#ifndef _KWP_H
#define _KWP_H
#include <Arduino.h>
#include <stdint.h>


// #define DBG_KWP2K  1

#define KWP_SID_STARTCOMMUNICATION          0x81
#define KWP_SID_STARTCOMMUNICATION_POS      0x81+0x40

#define KWP_SID_READECUIDENTIFICATION       0x1A
#define KWP_SID_READECUIDENTIFICATION_POS   0x1A+0x40

#define KWP_SID_READDATABYLOCALID           0x21
#define KWP_SID_READDATABYLOCALID_POS       0x21+0x40

#define KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS      0x18
#define KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS_POS  0x18+0x40

#define KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE       0x14
#define KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE_POS   0x14+0x40

#define KWP_SID_INPUTOUTPUTCONTROLBYLOCALID 0x30
#define KWP_SID_INPUTOUTPUTCONTROLBYLOCALID_POS 0x30+0x40

#define KWP_SID_TESTERPRESENT               0x3E
#define KWP_SID_TESTERPRESENT_POS           0x3E+0x40

#define KWP_SID_ERR                         0x7F
#define KWP_NRC_RESPONSE_PENDING            0x78

#define KWP2K_DEFAULT_TESTERADDRESS 0xF1
#define KWP2k_DEFAULT_ECUADDRESS    0x2B

#define KWP2K_BAUDRATE              10400
#define KWP2K_SESSION_TIMEOUT       3000        // after 3sec of silence we need to reinitialize
#define KWP2K_KEEPALIVE_INTERVAL    2000        // TesterPresent after 2sec idle (only while a web client is connected)

#define KWP2K_REPLY_TIMEOUT         2000        // after 2sec of not receiving a reply, we give up
#define KWP2K_PENDING_TIMEOUT       5000        // extended wait after a 0x78 "response pending"
#define KWP2K_MAX_PENDING           6           // at most 6 "response pending" in a row (30 s), then give up
#define KWP2K_QUIET_MAX             200         // ms: longest wait for the line to go quiet before a resend
#define KWP2K_INTERBYTE_TIMEOUT     100         // gap inside a frame that means garbage, resync (P1max is 20ms; kept loose)
#define KWP2K_ECHO_TIMEOUT          10          // ms to wait for our own bytes to come back from the K-line
#define KWP2K_MAX_RETRIES           1           // resend once after a corrupt reply

#define KWP2K_INTERMSG_DELAY        25          // enforced delay between sending a message after receiving one

// header modes
#define KWPMSG_HM2                  2           // Header with address information, physical target address

// TX Queue size
#define KWP2K_TX_QUEUE_SIZE         10

#define KWP2K_MAX_PAYLOAD           255         // SId + data


typedef struct KWP2KMessageStruct {
  uint8_t Fmt;
  uint8_t Tgt;
  uint8_t Src;
  uint8_t Len;                        // payload length (SId + data)
  uint8_t Payload[KWP2K_MAX_PAYLOAD]; // Payload[0] = SId
  uint8_t CS;
} KWP2KMessageStruct;

// K-line counters, shown on /log.html
typedef struct KWP2KStats {
  uint32_t tx, rx_ok, cs_err, timeouts, echo_err, resync, retries, inits, dropped;
} KWP2KStats;
extern KWP2KStats kwp_stats;

enum KWP2KMessageState { IDLE, QUEUED, SENT, RESPONSE_RECEIVED, RESPONSE_CORRUPT };

class KWP2KMsg
{
    public:
        KWP2KMsg(void);
        void setFmt(uint8_t header_mode, uint8_t datafield_len);
        void setSrc(uint8_t addr);
        void setTgt(uint8_t addr);
        void setSId(uint8_t sid);
        void setData(uint8_t idx, uint8_t value);
        uint8_t getCS(void);
        uint8_t calcCS(void);
        void print(void);
        uint8_t getSId(void);
        uint8_t getSrc(void);
        uint8_t getTgt(void);
        uint16_t getBytes(uint8_t *buf);          // fills buf (>= 260 bytes), returns length
        uint16_t getBytesLen(void);
        const uint8_t *getPayloadBytes(void);     // points into the message, nothing to free
        uint8_t getPayloadBytesLen(void);
        bool sameAs(KWP2KMsg *other);
        void setState(KWP2KMessageState state);
        KWP2KMessageState getState();
        void rxByte(uint8_t byte);
        bool inProgress(void);
        void reset(void);

    private:
        KWP2KMessageStruct _msg;
        KWP2KMessageState _state;
        uint16_t _idx;
        uint8_t _hdr_len;                         // 3 or 4 (with length byte)
};


class KWP2K
{
    public:
        KWP2K(uint8_t tx_pin, uint8_t rx_pin);
        KWP2K(uint8_t tx_pin, uint8_t rx_pin, uint8_t ECUaddress);
        KWP2K(uint8_t tx_pin, uint8_t rx_pin, uint8_t ECUaddress, uint8_t Testeraddress);
        void begin(void);                         // call from setup()
        void setECUaddress(uint8_t ECUaddress);
        void setTesteraddress(uint8_t Testeraddress);
        KWP2KMsg *getStartCommunicationMsg();
        void process(void);
        void setCallback(uint8_t SId, void (*callback)(const uint8_t *payload_bytes, uint8_t payload_len));
        void setTimeoutCallback(void (*callback)(void));
        void setKeepAlive(bool enabled);          // main.cpp: true while a web client is connected
        // ECU Functions
        void StartCommunication(void);
        void ReadECUIdentification(uint8_t option);
        void ReadDataByLocalId(uint8_t record, uint16_t dtc);
        void ReadDiagnosticTroubleCodesByStatus(void);
        void ClearDiagnosticInformationService(void);
        void InputOutputControlByLocalId(uint8_t id, uint8_t ctrl_param, uint8_t ctrl_state);


    private:
        void init(uint8_t tx_pin, uint8_t rx_pin, uint8_t ECUaddress, uint8_t Testeraddress);
        void fastInit(void);
        void resetFlags(void);
        void sendMsg(KWP2KMsg *msg);
        void dispatch(void);
        bool pushTXQueue(KWP2KMsg *msg);
        KWP2KMsg *popTXQueue(void);
        KWP2KMsg *peekTXQueue(void);
        void clearTXQueue(void);

        void (*cb_Timeout)(void);
        void (*cb_ErrorMessage)(const uint8_t *payload_bytes, uint8_t payload_len);
        void (*cb_StartCommunication)(const uint8_t *payload_bytes, uint8_t payload_len);
        void (*cb_ReadECUIdentification)(const uint8_t *payload_bytes, uint8_t payload_len);
        void (*cb_ReadDataByLocalId)(const uint8_t *payload_bytes, uint8_t payload_len);
        void (*cb_ReadDiagnosticTroubleCodesByStatus)(const uint8_t *payload_bytes, uint8_t payload_len);
        void (*cb_ClearDiagnosticInformationService)(const uint8_t *payload_bytes, uint8_t payload_len);
        void (*cb_InputOutputControlByLocalId)(const uint8_t *payload_bytes, uint8_t payload_len);


        // Serial Port (hardware UART0 on GPIO1/GPIO3)
        HardwareSerial *_ser;
        uint8_t _tx_pin;
        uint8_t _rx_pin;

        // ECU Addressing
        uint8_t _ECUaddress;
        uint8_t _Testeraddress;

        // FIFO of messages to be sent
        KWP2KMsg *_tx_queue[KWP2K_TX_QUEUE_SIZE];
        uint8_t _tx_head;
        uint8_t _tx_count;

        // last sent message (kept for one retry)
        KWP2KMsg _msgtx;
        uint8_t _retries;
        uint8_t _pending_count;                   // "response pending" replies to the current request
        bool _sent_is_keepalive;

        bool _waiting_for_reply;
        uint32_t _waiting_for_reply_ts;
        uint32_t _reply_timeout;

        // Buffer for receiving messages
        KWP2KMsg _msgrx;
        uint32_t _last_rx_byte_ts;

        // fast init, slow init established ?
        bool _is_initialized;
        uint32_t _last_tx_msg_ts;
        uint32_t _last_rx_msg_ts;

        // CommunicationStarted (Session still alive)
        bool _is_communication_started;

        // TesterPresent keep-alive
        bool _keepalive_enabled;
        bool _keepalive_supported;
};



#endif /* _KWP_H */
