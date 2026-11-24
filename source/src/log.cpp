/*
 * Lotus ABS ECU Tool
 * v1.0 (c) 2021 by CyberNet - original author
 * v1.1 (c) 2026 Updates and Bug Fixes by Rstone72
 *
 * >> Loggingfunctions <<
 *
 * v1.1: everything logged also goes to a RAM ring buffer that the web UI shows on /log.html
 */

#include <Arduino.h>
#include <stdint.h>
#include "mqtt.h"
#include "log.h"

bool log_serial_enabled = false;        // keep false: the serial port IS the K-line
bool log_mqtt_enabled = false;          // enable this for MQTT based logging (see mqtt.h for server config)

#define LOG_BUF_SIZE 6144
static char log_buf[LOG_BUF_SIZE];
static uint32_t log_written = 0;        // total bytes ever written (position of the next byte)

static void log_append(const char *s)
{
    while (*s)
    {
        log_buf[log_written % LOG_BUF_SIZE] = *s++;
        log_written++;
    }
}

static void log_out(const char *buffer)
{
    log_append(buffer);
    if (log_serial_enabled)
        Serial.print(buffer);          
    if (log_mqtt_enabled)
        mqtt_log("msg", buffer);            
}

// log message to all enabled sinks
void log_msg(const char *fmt, ...)
{
    char buffer[512];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_out(buffer);
}

static int log_stamp(char *buf, size_t size)
{
    uint32_t ms = millis();
    return snprintf(buf, size, "[%5lu.%03lu] ", (unsigned long)(ms/1000), (unsigned long)(ms%1000));
}

void log_line(const char *fmt, ...)
{
    char buffer[256];
    int n = log_stamp(buffer, sizeof(buffer));

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer+n, sizeof(buffer)-n-1, fmt, args);
    va_end(args);
    strcat(buffer, "\n");
    log_out(buffer);
}

void log_hex(const char *prefix, const uint8_t *data, uint16_t len)
{
    char buffer[64+3*48];
    int n = log_stamp(buffer, sizeof(buffer));
    n += snprintf(buffer+n, sizeof(buffer)-n, "%s:", prefix);
    uint16_t shown = (len > 48) ? 48 : len;
    for (uint16_t i=0; i<shown; i++)
        n += snprintf(buffer+n, sizeof(buffer)-n, " %02X", data[i]);
    if (shown < len)
        n += snprintf(buffer+n, sizeof(buffer)-n, " ... (%u bytes)", len);
    snprintf(buffer+n, sizeof(buffer)-n, "\n");
    log_out(buffer);
}

// copy the last (n-1) bytes of the log into buf, terminated
void log_tail(char *buf, size_t n)
{
    uint32_t len = (log_written < n-1) ? log_written : n-1;
    if (len > LOG_BUF_SIZE) len = LOG_BUF_SIZE;
    for (uint32_t i=0; i<len; i++)
        buf[i] = log_buf[(log_written - len + i) % LOG_BUF_SIZE];
    buf[len] = 0;
}

uint32_t log_total(void)
{
    return(log_written);
}

String log_read_since(uint32_t from)
{
    String out;
    if (from > log_written) from = 0;                           // module restarted since the page last asked
    if (log_written - from > LOG_BUF_SIZE) from = log_written - LOG_BUF_SIZE;   // older text was overwritten
    out.reserve(log_written - from);
    for (uint32_t i=from; i<log_written; i++)
        out += log_buf[i % LOG_BUF_SIZE];
    return(out);
}
