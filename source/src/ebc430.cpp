/*
 * Lotus ABS ECU Tool
 * v1.0 (c) 2021 by CyberNet - original author
 * v1.1 (c) 2026 Updates and Bug Fixes by Rstone72
 *
 * >> Callback functions for ECU replies, decode data and feed websocket back to the webapp <<
 *
 * v1.1: every reply is length-checked before it is read, ECU id strings are always terminated,
 *       wheel speed decoding fixed, JSON buffers sized for their content
 */


#include "log.h"
#include "kwp.h"
#include "web.h"
#include "ebc430.h"

char ecu_id_designcode[12];     // Design Code A120J6000F_
char ecu_id_hardware[7];        // Hardware EBC430
char ecu_id_software[12];       // Software LCFWDQ04K2K
char ecu_id_date[10];           // Date of Production 03/02/25
float ecu_voltage;              // Voltage
float speed_fl;                 // Front Left Wheel kph
float speed_fr;                 // Front Right Wheel kph
float speed_rl;                 // Rear Left Wheel kph
float speed_rr;                 // Rear Right Wheel kph

// copy 'len' bytes from 'src' (only as far as the reply reaches) into a terminated string
static void copy_id(char *dst, size_t dst_size, const uint8_t *payload, uint8_t payload_len, uint8_t offset, uint8_t len)
{
    size_t n = 0;
    if (offset < payload_len)
    {
        n = payload_len - offset;
        if (n > len) n = len;
        if (n > dst_size-1) n = dst_size-1;
        memcpy(dst, payload+offset, n);
    }
    dst[n] = 0;
}

static void ws_send(DynamicJsonDocument &doc)
{
    if (doc.overflowed())
        log_line("EBC: JSON reply truncated (buffer %u bytes)", (unsigned)doc.capacity());
    String json_string;
    serializeJson(doc, json_string);
    ws_server.broadcastTXT(json_string);
}

/*
 * A call to the ECU has not produced a response
 */
void TimeoutCB(void)
{        
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + 32);
    doc["sid"] = 0x7F;          // negative response is always 0x7F    
    doc["call_sid"] = 0;
    doc["errcode"] = 0xFF;      // timeout = 0xFF
    ws_send(doc);
}

void ErrorMessageCB(const uint8_t *payload_bytes, uint8_t payload_len)
{        
    if (payload_len < 3)
    {
        log_line("EBC: short negative response (%d bytes)", payload_len);
        return;
    }
    log_line("EBC: ECU refused SId=%02X, code %02X", payload_bytes[1], payload_bytes[2]);

    DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + 32);
    doc["sid"] = String(payload_bytes[0]);         // negative response is always 0x7F    
    doc["call_sid"] = String(payload_bytes[1]);    // the SId that was called for which the error is now shown
    doc["errcode"] = String(payload_bytes[2]);     // error code
    ws_send(doc);
}

void StartCommunicationCB(const uint8_t *payload_bytes, uint8_t payload_len)
{        
    log_line("EBC: ECU connected");
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + 32);
    doc["sid"] = String(payload_bytes[0]-0x40);         // response SId-0x40 = request SId
    ws_send(doc);
}


void ReadECUIdentificationCB(const uint8_t *payload_bytes, uint8_t payload_len)
{        
    if (payload_len < 2)
    {
        log_line("EBC: short ECU identification reply (%d bytes)", payload_len);
        return;
    }

    uint8_t option = payload_bytes[1];
    switch (option)
    {
        case EBC_OPT_DESIGN_CODE:   copy_id(ecu_id_designcode, sizeof(ecu_id_designcode), payload_bytes, payload_len, 2, 255); break;
        case EBC_OPT_HARDWARE_VER:  copy_id(ecu_id_hardware,   sizeof(ecu_id_hardware),   payload_bytes, payload_len, 2, 255); break;
        case EBC_OPT_SOFTWARE_VER:  copy_id(ecu_id_software,   sizeof(ecu_id_software),   payload_bytes, payload_len, 2, 255); break;
        case EBC_OPT_PRODDATE:      copy_id(ecu_id_date,       sizeof(ecu_id_date),       payload_bytes, payload_len, 2, 255); break;
        case EBC_OPT_ALL:           copy_id(ecu_id_designcode, sizeof(ecu_id_designcode), payload_bytes, payload_len, 2, 11);
                                    copy_id(ecu_id_hardware,   sizeof(ecu_id_hardware),   payload_bytes, payload_len, 13, 6);
                                    copy_id(ecu_id_software,   sizeof(ecu_id_software),   payload_bytes, payload_len, 19, 11);
                                    copy_id(ecu_id_date,       sizeof(ecu_id_date),       payload_bytes, payload_len, 30, 9);
                                    if (payload_len < 39)
                                        log_line("EBC: ECU identification shorter than expected (%d bytes)", payload_len);
                                    break;
    }
    log_line("EBC: id '%s' '%s' '%s' '%s'", ecu_id_designcode, ecu_id_hardware, ecu_id_software, ecu_id_date);

    DynamicJsonDocument doc(JSON_OBJECT_SIZE(5) + 96);
    doc["sid"] = String(payload_bytes[0]-0x40);         // response SId-0x40 = request SId
    doc["ecu_id_designcode"] = ecu_id_designcode;
    doc["ecu_id_hardware"] = ecu_id_hardware;
    doc["ecu_id_software"] = ecu_id_software;
    doc["ecu_id_date"] = ecu_id_date;
    ws_send(doc);
}


void ReadDataByLocalIdCB(const uint8_t *payload_bytes, uint8_t payload_len)
{        
    if (payload_len < 3)
    {
        log_line("EBC: short ReadDataByLocalId reply (%d bytes)", payload_len);
        return;
    }

    DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + 48);
    doc["sid"] = String(payload_bytes[0]-0x40);         // response SId-0x40 = request SId
    doc["record"] = payload_bytes[1];    

    uint8_t record = payload_bytes[1];
    // wheel speeds: 16 bit, high byte first, 1/256 kph per bit
    float speed = (payload_len >= 4) ? (float)((payload_bytes[2]<<8) | payload_bytes[3]) / 256.0f : 0.0f;
    switch (record)
    {
        case EBC_REC_VOLTAGE:   ecu_voltage = (float)(payload_bytes[2]*0.1); doc["value"]=String(ecu_voltage); break;
        case EBC_REC_SPEED_FL:  speed_fl = speed; doc["value"]=String(speed, 1); break;
        case EBC_REC_SPEED_FR:  speed_fr = speed; doc["value"]=String(speed, 1); break;
        case EBC_REC_SPEED_RL:  speed_rl = speed; doc["value"]=String(speed, 1); break;
        case EBC_REC_SPEED_RR:  speed_rr = speed; doc["value"]=String(speed, 1); break;
    }
    ws_send(doc);
}

void ReadDiagnosticTroubleCodeByStatusCB(const uint8_t *payload_bytes, uint8_t payload_len)
{        
    if (payload_len < 2)
    {
        log_line("EBC: short DTC reply (%d bytes)", payload_len);
        return;
    }
    uint8_t dtc_count = payload_bytes[1];
    uint8_t dtc_avail = (payload_len - 2) / 3;
    if (dtc_count > dtc_avail)
    {
        log_line("EBC: ECU reports %d DTCs but sent %d", dtc_count, dtc_avail);
        dtc_count = dtc_avail;
    }
    log_line("EBC: %d DTC(s)", dtc_count);

    uint8_t dtc_idx, dtc_type_code;
    char dtc_type_char;
    uint16_t dtc_code;
    uint8_t dtc_status;

    // json array that holds all DTCs    
    // sized with ArduinoJson's own macros so it is right on any CPU (+16 bytes of copied strings per DTC)
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(dtc_count) + dtc_count*(JSON_OBJECT_SIZE(4) + 16) + 32);
    doc["sid"] = String(payload_bytes[0]-0x40);         // response SId-0x40 = request SId
    doc["dtc_count"] = dtc_count;    

    JsonArray dtc_array = doc.createNestedArray("dtcs");

    for (dtc_idx=0 ; dtc_idx<dtc_count ; dtc_idx++)
    {
        const uint8_t *d = payload_bytes + 2 + 3*dtc_idx;
        dtc_type_code = (d[0] >> 6)&0x3;       // the two MSB's encode the component type (P)owertrain, (C)hassis, (B)ody, (U)Network        
        switch (dtc_type_code)
        {
            case 0x0: dtc_type_char='P'; break;
            case 0x1: dtc_type_char='C'; break;
            case 0x2: dtc_type_char='B'; break;
            case 0x3: dtc_type_char='U'; break;
            default: dtc_type_char='?';
        }

        dtc_code = (d[0]<<8) | d[1];
        dtc_code &= 0x3FFF;    // mask off dtc type code bits        
        dtc_status = d[2];
        char dtc_str[8];
        snprintf(dtc_str, sizeof(dtc_str), "%c%04x", dtc_type_char, dtc_code);
        char type_str[2] = { dtc_type_char, 0 };

        JsonObject single_dtc = dtc_array.createNestedObject(); 
        single_dtc["dtc_code"] = dtc_code;
        single_dtc["dtc_type_char"] = type_str;
        single_dtc["dtc_str"] = dtc_str;
        single_dtc["dtc_status"] = dtc_status;              
    }
    ws_send(doc);
}


void ClearDiagnosticInformationServiceCB(const uint8_t *payload_bytes, uint8_t payload_len)
{        
    log_line("EBC: DTCs cleared");
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(1) + 32);
    doc["sid"] = String(payload_bytes[0]-0x40);         // tell the page, so it can confirm and re-read
    ws_send(doc);
}

void InputOutputControlByLocalIdCB(const uint8_t *payload_bytes, uint8_t payload_len)
{
    if (payload_len < 3)
    {
        log_line("EBC: short IO control reply (%d bytes)", payload_len);
        return;
    }

    DynamicJsonDocument doc(JSON_OBJECT_SIZE(4) + 48);
    doc["sid"] =  String(payload_bytes[0]-0x40);
    doc["id"] = String(payload_bytes[1]);             // IO local identifier
    doc["ctrl_param"] = String(payload_bytes[2]);     // Control Parameter
    if ((payload_bytes[2] == 0x1) && (payload_len >= 4))
    {
        doc["ctrl_state"] = String(payload_bytes[3]);     // Control State (only present if param is 0x1 = return current status)
    }
    ws_send(doc);
}
