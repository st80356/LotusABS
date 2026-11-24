/*
 * Lotus ABS ECU Tool
 * v1.0 (c) 2021 by CyberNet - original author
 * v1.1 (c) 2026 Updates and Bug Fixes by Rstone72
 *
 * >> Poor mans KWP2k implementation, as async as possible to not waste any cycles while waiting <<
 *
 * v1.1 changes:
 *  - K-line on the hardware UART (was SoftwareSerial on the same pins)
 *  - no heap leaks: replies are handed to callbacks as a pointer into the rx buffer
 *  - FIFO tx queue, duplicate requests dropped, nothing leaks when the queue is full
 *  - receiver resyncs on garbage / inter-byte gaps, checks addresses, supports the length byte
 *  - corrupt reply -> one resend, then error straight away (was: 2 s wait, then timeout)
 *  - 0x7F/0x78 "response pending" extends the wait instead of being shown as an error
 *  - optional TesterPresent keep-alive while a web client is connected
 *  - every frame and every error goes to the debug log (/log.html)
 *  - safety lock: only the ABS address, physical addressing and a fixed list of services ever
 *    reach the K-line (the line is shared with other modules, e.g. the airbag ECU)
 *  - no wait can run forever: line-quiet wait before a resend capped at 200 ms,
 *    at most 6 "response pending" replies in a row
 */

#include <Arduino.h>
#include "kwp.h"
#include "log.h"

KWP2KStats kwp_stats;

// constructor and overloads
KWP2K::KWP2K(uint8_t tx_pin, uint8_t rx_pin)
{
    this->init(tx_pin, rx_pin, KWP2k_DEFAULT_ECUADDRESS, KWP2K_DEFAULT_TESTERADDRESS);
}

KWP2K::KWP2K(uint8_t tx_pin, uint8_t rx_pin, uint8_t ECUaddress)
{
    this->init(tx_pin, rx_pin, ECUaddress, KWP2K_DEFAULT_TESTERADDRESS);
}

KWP2K::KWP2K(uint8_t tx_pin, uint8_t rx_pin, uint8_t ECUaddress, uint8_t Testeraddress)
{
    this->init(tx_pin, rx_pin, ECUaddress, Testeraddress);
}

void KWP2K::init(uint8_t tx_pin, uint8_t rx_pin, uint8_t ECUaddress, uint8_t Testeraddress)
{
    _tx_pin = tx_pin;
    _rx_pin = rx_pin;
    _ECUaddress = ECUaddress;
    _Testeraddress = Testeraddress;
    _ser = &Serial;
    cb_Timeout = nullptr;
    cb_ErrorMessage = nullptr;
    cb_StartCommunication = nullptr;
    cb_ReadECUIdentification = nullptr;
    cb_ReadDataByLocalId = nullptr;
    cb_ReadDiagnosticTroubleCodesByStatus = nullptr;
    cb_ClearDiagnosticInformationService = nullptr;
    cb_InputOutputControlByLocalId = nullptr;
    _tx_head = 0;
    _tx_count = 0;
    for (uint8_t i=0;i<KWP2K_TX_QUEUE_SIZE;i++) _tx_queue[i]=NULL;
    _keepalive_enabled = false;
    _keepalive_supported = true;
    this->resetFlags();
}

// start the UART (call from setup(), not from a global constructor)
void KWP2K::begin(void)
{
    _ser->begin(KWP2K_BAUDRATE, SERIAL_8N1);
    while (_ser->available()) _ser->read();
}

void KWP2K::resetFlags()
{
    _is_initialized = false;
    _is_communication_started = false;
    _last_tx_msg_ts = 0;           // never
    _last_rx_msg_ts = 0;           // never
    _last_rx_byte_ts = 0;
    _waiting_for_reply = false;
    _retries = 0;
    _pending_count = 0;
    _sent_is_keepalive = false;
    _reply_timeout = KWP2K_REPLY_TIMEOUT;
}

void KWP2K::setTimeoutCallback(void (*callback)(void))
{
    cb_Timeout = callback;
}

void KWP2K::setKeepAlive(bool enabled)
{
    _keepalive_enabled = enabled;
}

void KWP2K::setCallback(uint8_t SId, void (*callback)(const uint8_t *payload_bytes, uint8_t payload_len))
{
    switch (SId)
    {
        case KWP_SID_ERR:                                   cb_ErrorMessage                         = callback; break;
        case KWP_SID_STARTCOMMUNICATION:                    cb_StartCommunication                   = callback; break;
        case KWP_SID_READECUIDENTIFICATION:                 cb_ReadECUIdentification                = callback; break;
        case KWP_SID_READDATABYLOCALID:                     cb_ReadDataByLocalId                    = callback; break;
        case KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS:    cb_ReadDiagnosticTroubleCodesByStatus   = callback; break;
        case KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE:     cb_ClearDiagnosticInformationService    = callback; break;
        case KWP_SID_INPUTOUTPUTCONTROLBYLOCALID:           cb_InputOutputControlByLocalId          = callback; break;
    }
}

/*
* add KWP2KMsg to the end of the TX FIFO
* the queue takes ownership of msg: it is deleted if it can't be queued
*/
bool KWP2K::pushTXQueue(KWP2KMsg *msg)
{
    // same request already waiting? (web pages poll; with the ECU off they would pile up)
    for (uint8_t n=0; n<_tx_count; n++)
    {
        KWP2KMsg *q = _tx_queue[(_tx_head+n) % KWP2K_TX_QUEUE_SIZE];
        if (q->sameAs(msg))
        {
            delete msg;
            return(true);
        }
    }
    if (_tx_count >= KWP2K_TX_QUEUE_SIZE)
    {
        log_line("KWP: tx queue full, request SId=%02X dropped", msg->getSId());
        kwp_stats.dropped++;
        delete msg;
        return(false);
    }
    _tx_queue[(_tx_head+_tx_count) % KWP2K_TX_QUEUE_SIZE] = msg;
    _tx_count++;
    return(true);
}

/*
* pop oldest KWP2KMsg from TX FIFO (caller deletes it)
*/
KWP2KMsg *KWP2K::popTXQueue(void)
{
    if (_tx_count == 0) return(NULL);
    KWP2KMsg *retptr = _tx_queue[_tx_head];
    _tx_queue[_tx_head] = NULL;
    _tx_head = (_tx_head+1) % KWP2K_TX_QUEUE_SIZE;
    _tx_count--;
    return(retptr);
}

/*
* drop everything still queued (ECU not answering: don't send old requests,
* e.g. a valve or pump command, minutes later when the ignition comes on)
*/
void KWP2K::clearTXQueue(void)
{
    // only when the ECU did not even answer StartCommunication (ignition off, cable out):
    // a single lost reply inside a running session must not throw away the next clicks
    if (_msgtx.getSId() != KWP_SID_STARTCOMMUNICATION) return;

    KWP2KMsg *keep[KWP2K_TX_QUEUE_SIZE];
    uint8_t nkeep = 0, n = 0;
    KWP2KMsg *m;
    while ((m = this->popTXQueue()) != NULL)
    {
        const uint8_t *p = m->getPayloadBytes();
        // an "OFF" for a valve, the pump or the relay is always safe to deliver later: keep it
        bool is_off = (p[0] == KWP_SID_INPUTOUTPUTCONTROLBYLOCALID) && (m->getPayloadBytesLen() >= 4) && (p[2] == 0x07) && (p[3] == 0x00);
        if (is_off) keep[nkeep++] = m;
        else { delete m; n++; }
    }
    for (uint8_t i=0; i<nkeep; i++) this->pushTXQueue(keep[i]);
    if (n) log_line("KWP: ECU not answering, %d queued request(s) cancelled", n);
}

/*
* peek oldest KWP2KMsg in TX FIFO
*/
KWP2KMsg *KWP2K::peekTXQueue(void)
{
    if (_tx_count == 0) return(NULL);
    return(_tx_queue[_tx_head]);
}

//////////////////////////////////////////////
// Process KWP2K Handler
//////////////////////////////////////////////
void KWP2K::process()
{
    uint32_t now = millis();

    // session timed out ?
    if (_is_initialized && !_waiting_for_reply && (now - _last_tx_msg_ts > KWP2K_SESSION_TIMEOUT))
    {
        log_line("KWP: session idle, will re-init on next request");
        _is_initialized = false;           // Communication Session no longer initialized
        _is_communication_started = false; // Communication Session timed out
    }

    // not waiting: anything on the line now is noise, don't let it leak into the next reply
    // (counted and logged at most once a second, so a noisy line cannot flood the debug log)
    static uint32_t stray = 0, stray_logged_ms = 0;
    if (!_waiting_for_reply && _ser->available())
    {
        while (_ser->available()) { _ser->read(); stray++; }
    }
    if (stray && (now - stray_logged_ms >= 1000))
    {
        log_line("KWP: %lu stray byte(s) discarded", (unsigned long)stray);
        stray = 0;
        stray_logged_ms = now;
    }

    // send next queued message
    if ( (now - _last_rx_msg_ts > KWP2K_INTERMSG_DELAY) &&       // wait for intermessage delay (between last rx and next tx)
         !_waiting_for_reply &&                                  // we dont have a reply that we expect pending
         this->peekTXQueue()                                     // there is a new message waiting to be sent
       )
    {
        KWP2KMsg *msg;
        // a new session always starts with the fast-init wake-up (the ECU may have dropped the old one,
        // e.g. ignition cycled, while we still thought the line was initialized)
        if (!_is_initialized || !_is_communication_started)
        {
            this->fastInit();
        }
        // do we have a comm session
        if (!_is_communication_started && (this->peekTXQueue()->getSId()!=KWP_SID_STARTCOMMUNICATION))
        {
            msg = this->getStartCommunicationMsg();     // send the StartCommunication message first
        }
        else
        {
            msg = this->popTXQueue();                   // send the pending message
        }
        _msgtx = *msg;
        delete msg;
        _retries = 0;
        _sent_is_keepalive = false;
        this->sendMsg(&_msgtx);
    }
    // keep-alive: session up, web client connected, nothing else to do
    else if ( _keepalive_enabled && _keepalive_supported && _is_communication_started &&
              !_waiting_for_reply && !this->peekTXQueue() &&
              (now - _last_tx_msg_ts > KWP2K_KEEPALIVE_INTERVAL) &&
              (now - _last_rx_msg_ts > KWP2K_INTERMSG_DELAY) )
    {
        _msgtx.reset();
        _msgtx.setFmt(KWPMSG_HM2, 1);
        _msgtx.setSrc(_Testeraddress);
        _msgtx.setTgt(_ECUaddress);
        _msgtx.setSId(KWP_SID_TESTERPRESENT);
        _retries = KWP2K_MAX_RETRIES;           // no resend for keep-alives
        _sent_is_keepalive = true;
        this->sendMsg(&_msgtx);
    }

    // are we waiting for a reply ?
    if (_waiting_for_reply)
    {
        while (_waiting_for_reply && _ser->available())
        {
            uint8_t in = _ser->read();
            now = millis();
            // a gap inside a frame means we are out of step: start over with this byte
            if (_msgrx.inProgress() && (now - _last_rx_byte_ts > KWP2K_INTERBYTE_TIMEOUT))
            {
                log_line("KWP: %dms gap inside reply, resync", (int)(now - _last_rx_byte_ts));
                kwp_stats.resync++;
                _msgrx.reset();
            }
            _last_rx_byte_ts = now;
            _last_rx_msg_ts = now;
            _msgrx.rxByte(in);

            if (_msgrx.getState() == RESPONSE_RECEIVED)
            {
                // v1.0 never checked the addresses; to stay as tolerant, an unexpected address is only logged
                static bool addr_noted = false;           // log it once, not on every reply (fills the log)
                if (!addr_noted && ((_msgrx.getTgt() != _Testeraddress) || (_msgrx.getSrc() != _ECUaddress)))
                {
                    addr_noted = true;
                    log_line("KWP: note: ECU replies addressed %02X from %02X (logged once)", _msgrx.getTgt(), _msgrx.getSrc());
                }
                const uint8_t *p = _msgrx.getPayloadBytes();
                if ((p[0] == KWP_SID_ERR) && (_msgrx.getPayloadBytesLen() >= 3) && (p[2] == KWP_NRC_RESPONSE_PENDING) &&
                    (_pending_count < KWP2K_MAX_PENDING))
                {
                    _pending_count++;
                    log_line("KWP: ECU busy (response pending %d), waiting", _pending_count);
                    _waiting_for_reply_ts = now;
                    _reply_timeout = KWP2K_PENDING_TIMEOUT;
                    _msgrx.reset();
                    continue;
                }
                kwp_stats.rx_ok++;
                this->dispatch();
                _waiting_for_reply = false;
                _msgrx.reset();
            }
            else if (_msgrx.getState() == RESPONSE_CORRUPT)
            {
                kwp_stats.cs_err++;
                if (_retries < KWP2K_MAX_RETRIES)
                {
                    _retries++;
                    // let the line go quiet (P2min 25ms), then resend the same request;
                    // never wait more than KWP2K_QUIET_MAX in total (a line that never goes quiet
                    // used to keep the module stuck here, web pages dead until power-off)
                    uint32_t t = millis(), t0 = t;
                    while ((millis() - t < 30) && (millis() - t0 < KWP2K_QUIET_MAX)) { if (_ser->available()) { _ser->read(); t = millis(); } yield(); }
                    if (millis() - t0 >= KWP2K_QUIET_MAX) log_line("KWP: line never went quiet (noise on the K-line?)");
                    log_line("KWP: resending SId=%02X", _msgtx.getSId());
                    kwp_stats.retries++;
                    this->sendMsg(&_msgtx);
                }
                else
                {
                    log_line("KWP: reply corrupt again, giving up on SId=%02X", _msgtx.getSId());
                    _waiting_for_reply = false;
                    _is_communication_started = false;
                    _is_initialized = false;
                    _msgrx.reset();
                    if (!_sent_is_keepalive) this->clearTXQueue();
                    if (!_sent_is_keepalive && cb_Timeout) this->cb_Timeout();
                }
            }
        }

        // have we been waiting too long ?
        if (_waiting_for_reply && (millis() - _waiting_for_reply_ts > _reply_timeout))
        {
            _waiting_for_reply = false;
            _is_communication_started = false;
            _is_initialized = false;
            kwp_stats.timeouts++;
            log_line("KWP: no reply to SId=%02X within %lums%s", _msgtx.getSId(), (unsigned long)_reply_timeout,
                     _msgrx.inProgress() ? " (partial frame received)" : "");
            _msgrx.reset();
            if (!_sent_is_keepalive) this->clearTXQueue();
            if (!_sent_is_keepalive && cb_Timeout) this->cb_Timeout();
        }
    }
}

// hand a complete, valid reply to its callback
void KWP2K::dispatch(void)
{
    const uint8_t *p = _msgrx.getPayloadBytes();
    uint8_t len = _msgrx.getPayloadBytesLen();

    if (_sent_is_keepalive)
    {
        if (p[0] == KWP_SID_ERR)
        {
            log_line("KWP: ECU rejects TesterPresent, keep-alive off");
            _keepalive_supported = false;
        }
        return;                                 // keep-alive replies never go to the web UI
    }

    switch(p[0])
    {
        case KWP_SID_ERR:                                       if (cb_ErrorMessage) this->cb_ErrorMessage(p, len); break;
        case KWP_SID_STARTCOMMUNICATION_POS:                    _is_communication_started=true; if (cb_StartCommunication) this->cb_StartCommunication(p, len); break;
        case KWP_SID_READECUIDENTIFICATION_POS:                 if (cb_ReadECUIdentification) this->cb_ReadECUIdentification(p, len); break;
        case KWP_SID_READDATABYLOCALID_POS:                     if (cb_ReadDataByLocalId) this->cb_ReadDataByLocalId(p, len); break;
        case KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS_POS:    if (cb_ReadDiagnosticTroubleCodesByStatus) this->cb_ReadDiagnosticTroubleCodesByStatus(p, len); break;
        case KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE_POS:     if (cb_ClearDiagnosticInformationService) this->cb_ClearDiagnosticInformationService(p, len); break;
        case KWP_SID_INPUTOUTPUTCONTROLBYLOCALID_POS:           if (cb_InputOutputControlByLocalId) this->cb_InputOutputControlByLocalId(p, len); break;
        default: log_line("KWP: unexpected reply SId=%02X", p[0]); break;
    }
}

// Fast Init
void KWP2K::fastInit()
{
    log_line("KWP: fast init");
    kwp_stats.inits++;
    _ser->flush();
    _ser->end();
    pinMode(_tx_pin, OUTPUT);
    digitalWrite (_tx_pin, HIGH);
    delay(500);                 // W5: bus idle
    digitalWrite (_tx_pin, LOW);
    delay(25);                  // TiniL
    digitalWrite (_tx_pin, HIGH);
    delay(25);                  // rest of TWuP
    _ser->begin(KWP2K_BAUDRATE, SERIAL_8N1);    // pins back to UART
    while (_ser->available()) _ser->read();     // drop the wake-up pattern echo
    // we should be initialized now, for the fastInit there is no way to know for sure
    _is_initialized = true;
}

// Functions

/*
* Get StartCommunication message (used for auto connect)
*/
KWP2KMsg *KWP2K::getStartCommunicationMsg()
{
    KWP2KMsg *msg;
    msg = new KWP2KMsg();
    msg->setFmt(KWPMSG_HM2, 1);
    msg->setSrc(_Testeraddress);
    msg->setTgt(_ECUaddress);
    msg->setSId(KWP_SID_STARTCOMMUNICATION);
    msg->setState(QUEUED);
    return(msg);
}

/*
* Send StartCommunication message
*/
void KWP2K::StartCommunication(void)
{
    this->pushTXQueue(this->getStartCommunicationMsg());
}


void KWP2K::ReadECUIdentification(uint8_t option)
{
    KWP2KMsg *msg;
    msg = new KWP2KMsg();
    msg->setFmt(KWPMSG_HM2, 2);
    msg->setSrc(_Testeraddress);
    msg->setTgt(_ECUaddress);
    msg->setSId(KWP_SID_READECUIDENTIFICATION);
    msg->setData(0, option);
    msg->setState(QUEUED);
    this->pushTXQueue(msg);
}

void KWP2K::ReadDataByLocalId(uint8_t record, uint16_t dtc)
{
    KWP2KMsg *msg;
    msg = new KWP2KMsg();
    if (record == 0x10)
        msg->setFmt(KWPMSG_HM2, 4);
    else
        msg->setFmt(KWPMSG_HM2, 2);
    msg->setSrc(_Testeraddress);
    msg->setTgt(_ECUaddress);
    msg->setSId(KWP_SID_READDATABYLOCALID);
    msg->setData(0, record);
    if (record == 0x10)                 // Local Identifier == 0x10 ?
    {
        msg->setData(1, dtc>>8);        // DTC Highbyte
        msg->setData(2, dtc&0xFF);      // DTC Lowbyte
    }
    msg->setState(QUEUED);
    this->pushTXQueue(msg);
}

void KWP2K::ReadDiagnosticTroubleCodesByStatus(void)
{
    KWP2KMsg *msg;
    msg = new KWP2KMsg();
    msg->setFmt(KWPMSG_HM2, 4);
    msg->setSrc(_Testeraddress);
    msg->setTgt(_ECUaddress);
    msg->setSId(KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS);
    msg->setData(0, 0x0);     // status OfDTC (independent of their status)
    msg->setData(1, 0xFF);    // Group (0xFF00) Highbyte
    msg->setData(2, 0x0);     // Group (0xFF00) Lowbyte
    msg->setState(QUEUED);
    this->pushTXQueue(msg);
}

void KWP2K::ClearDiagnosticInformationService(void)
{
    KWP2KMsg *msg;
    msg = new KWP2KMsg();
    msg->setFmt(KWPMSG_HM2, 3);
    msg->setSrc(_Testeraddress);
    msg->setTgt(_ECUaddress);
    msg->setSId(KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE);
    msg->setData(0, 0xFF);    // Group (0xFF00) Highbyte
    msg->setData(1, 0x0);     // Group (0xFF00) Lowbyte
    msg->setState(QUEUED);
    this->pushTXQueue(msg);
}

void KWP2K::InputOutputControlByLocalId(uint8_t id, uint8_t ctrl_param, uint8_t ctrl_state)
{
    KWP2KMsg *msg;
    msg = new KWP2KMsg();
    if (ctrl_param == 0x7)
        msg->setFmt(KWPMSG_HM2, 4);
    else
        msg->setFmt(KWPMSG_HM2, 3);

    msg->setSrc(_Testeraddress);
    msg->setTgt(_ECUaddress);
    msg->setSId(KWP_SID_INPUTOUTPUTCONTROLBYLOCALID);
    msg->setData(0, id);
    msg->setData(1, ctrl_param);
    if (ctrl_param == 0x7)
    {
        msg->setData(2, ctrl_state);
    }
    msg->setState(QUEUED);
    this->pushTXQueue(msg);
}

// Addressing
void KWP2K::setECUaddress(uint8_t ECUaddress)
{
    _ECUaddress = ECUaddress;
}
void KWP2K::setTesteraddress(uint8_t Testeraddress)
{
    _Testeraddress = Testeraddress;
}

// Send a message, check the K-line echo, start waiting for the reply
// safety lock: the K-line is shared with other modules (e.g. the airbag ECU). Only physically
// addressed frames to the ABS ECU, with one of these services, are ever put on the line.
static bool kwp_allowed(uint8_t fmt, uint8_t tgt, uint8_t ecu, uint8_t sid)
{
    if ((fmt & 0xC0) != 0x80) return(false);        // physical addressing only (never functional/broadcast)
    if (tgt != ecu) return(false);                  // ABS ECU only
    switch (sid)
    {
        case KWP_SID_STARTCOMMUNICATION:
        case KWP_SID_READECUIDENTIFICATION:
        case KWP_SID_READDATABYLOCALID:
        case KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS:
        case KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE:
        case KWP_SID_INPUTOUTPUTCONTROLBYLOCALID:
        case KWP_SID_TESTERPRESENT:
            return(true);
    }
    return(false);
}

void KWP2K::sendMsg(KWP2KMsg *msg)
{
    uint8_t data[KWP2K_MAX_PAYLOAD+5];
    uint16_t data_len = msg->getBytes(data);

    if (!kwp_allowed(data[0], data[1], _ECUaddress, msg->getSId()))
    {
        log_line("KWP: BLOCKED frame Fmt=%02X Tgt=%02X SId=%02X (safety lock: ABS ECU and known services only)", data[0], data[1], msg->getSId());
        _waiting_for_reply = false;
        return;
    }

    while (_ser->available()) _ser->read();     // nothing old in front of the echo
    _ser->write(data, data_len);
    _ser->flush();                              // wait until the last bit is out
    _last_tx_msg_ts = millis();
    kwp_stats.tx++;
    log_hex(_sent_is_keepalive ? "TX keep-alive" : "TX", data, data_len);

    // the K-line is one wire: everything we send comes straight back on RX
    uint16_t got = 0;
    bool match = true;
    uint32_t t = millis();
    // read the echo; bytes that already arrived are always taken, even if a Wi-Fi task held us up
    // past the timeout (only give up when nothing more is there)
    while (got < data_len)
    {
        if (_ser->available())
        {
            if ((uint8_t)_ser->read() != data[got]) match = false;
            got++;
        }
        else if (millis() - t >= KWP2K_ECHO_TIMEOUT) break;
        else yield();
    }
    if (got < data_len)
    {
        kwp_stats.echo_err++;
        log_line("KWP: echo missing (%d of %d bytes) - check L9637 / K-line wiring", got, data_len);
    }
    else if (!match)
    {
        kwp_stats.echo_err++;
        log_line("KWP: echo differs from sent bytes - bus collision or noise");
    }

    _msgrx.reset();
    _waiting_for_reply_ts = millis();
    _reply_timeout = KWP2K_REPLY_TIMEOUT;
    _pending_count = 0;
    _waiting_for_reply = true;
}


// ===============================================
// ================= KWP2KMsg ====================
// ==============================================

KWP2KMsg::KWP2KMsg(void)
{
   this->reset();
   _msg.Fmt = 0;
   _msg.Tgt = 0;
   _msg.Src = 0;
   _msg.Len = 0;
   _msg.CS = 0;
}

void KWP2KMsg::reset(void)
{
    _state = IDLE;
    _idx = 0;
    _hdr_len = 3;
}

bool KWP2KMsg::inProgress(void)
{
    return((_idx > 0) && (_state == IDLE));
}

void KWP2KMsg::setFmt(uint8_t header_mode, uint8_t datafield_len)
{
    header_mode = header_mode & 0x3;        // mask 2 bits
    datafield_len = datafield_len & 0x3F;   // mask 6 bits
    _msg.Fmt = datafield_len | (header_mode<<6);
    _msg.Len = datafield_len;
    _hdr_len = 3;
}

void KWP2KMsg::setSrc(uint8_t addr)
{
    _msg.Src = addr;
}

void KWP2KMsg::setTgt(uint8_t addr)
{
    _msg.Tgt = addr;
}

void KWP2KMsg::setSId(uint8_t sid)
{
    _msg.Payload[0] = sid;
}

uint8_t KWP2KMsg::getSId(void)
{
    return(_msg.Payload[0]);
}

uint8_t KWP2KMsg::getSrc(void)
{
    return(_msg.Src);
}

uint8_t KWP2KMsg::getTgt(void)
{
    return(_msg.Tgt);
}

uint8_t KWP2KMsg::getCS(void)
{
    return(_msg.CS);
}

uint8_t KWP2KMsg::calcCS(void)
{
    uint8_t sum = _msg.Fmt + _msg.Tgt + _msg.Src;
    if (_hdr_len == 4) sum += _msg.Len;
    for (uint16_t j=0; j<_msg.Len; j++)
    {
        sum += _msg.Payload[j];
    }
    return(sum);
}

/*
 * copy the complete frame (header, payload, checksum) to buf, return its length
 */
uint16_t KWP2KMsg::getBytes(uint8_t *buf)
{
    uint16_t i = 0;
    buf[i++] = _msg.Fmt;
    buf[i++] = _msg.Tgt;
    buf[i++] = _msg.Src;
    if (_hdr_len == 4) buf[i++] = _msg.Len;
    for (uint16_t j=0; j<_msg.Len; j++)
    {
        buf[i++] = _msg.Payload[j];
    }
    buf[i++] = this->calcCS();
    return(i);
}

/*
 * get payload length without any headers (SId + data)
 */
uint8_t KWP2KMsg::getPayloadBytesLen(void)
{
    return(_msg.Len);
}

/*
 * the payload bytes (SId and following bytes) - valid until the next reset()/rxByte()
 */
const uint8_t *KWP2KMsg::getPayloadBytes(void)
{
    return(_msg.Payload);
}

/*
 * get message length including CRC byte
 */
uint16_t KWP2KMsg::getBytesLen(void)
{
    return(_hdr_len + _msg.Len + 1);
}

/*
 * set payload data bytes (idx 0 = first byte after the SId)
 */
void KWP2KMsg::setData(uint8_t idx, uint8_t value)
{
    if (idx < KWP2K_MAX_PAYLOAD-1)
    {
        _msg.Payload[idx+1] = value;
    }
}

bool KWP2KMsg::sameAs(KWP2KMsg *other)
{
    if ((_msg.Fmt != other->_msg.Fmt) || (_msg.Tgt != other->_msg.Tgt) || (_msg.Src != other->_msg.Src) || (_msg.Len != other->_msg.Len))
        return(false);
    return(memcmp(_msg.Payload, other->_msg.Payload, _msg.Len) == 0);
}

void KWP2KMsg::print(void)
{
    uint8_t buf[KWP2K_MAX_PAYLOAD+5];
    uint16_t len = this->getBytes(buf);
    log_hex("MSG", buf, len);
}

void KWP2KMsg::setState(KWP2KMessageState state)
{
    _state = state;
}

/*
 * frame parser: Fmt Tgt Src [Len] SId data... CS
 */
void KWP2KMsg::rxByte(uint8_t byte)
{
    if (_state != IDLE) this->reset();          // previous frame done, start a new one

    if (_idx == 0)
    {
        // replies carry address information (format byte bit 7 set);
        // anything else is noise - drop it and keep looking for a header
        if ((byte & 0x80) == 0) return;
        _msg.Fmt = byte;
        _msg.Len = byte & 0x3F;
        _hdr_len = (_msg.Len == 0) ? 4 : 3;
        _idx++;
        return;
    }
    if (_idx == 1) { _msg.Tgt = byte; _idx++; return; }
    if (_idx == 2) { _msg.Src = byte; _idx++; return; }
    if ((_idx == 3) && (_hdr_len == 4))
    {
        _msg.Len = byte;
        if (_msg.Len == 0) { this->reset(); return; }
        _idx++;
        return;
    }

    uint16_t pos = _idx - _hdr_len;             // position after the header
    if (pos < _msg.Len)
    {
        _msg.Payload[pos] = byte;
        _idx++;
        return;
    }

    // checksum byte
    _msg.CS = byte;
    _idx++;
    if (_msg.CS == this->calcCS())
    {
        _state = RESPONSE_RECEIVED;
        uint8_t buf[KWP2K_MAX_PAYLOAD+5];
        uint16_t len = this->getBytes(buf);
        log_hex("RX", buf, len);
    }
    else
    {
        _state = RESPONSE_CORRUPT;
        log_line("KWP: bad checksum: received %02X, calculated %02X", _msg.CS, this->calcCS());
    }
}

KWP2KMessageState KWP2KMsg::getState()
{
    return(_state);
}
