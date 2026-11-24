#ifndef _LOG_H
#define _LOG_H

#include <Arduino.h>

#define FW_VERSION "1.1"

extern bool log_serial_enabled;
extern bool log_mqtt_enabled;

// raw log text (kept for the original debug messages)
void log_msg(const char *fmt, ...);
// one timestamped line: "[  12.345] text\n"
void log_line(const char *fmt, ...);
// one timestamped line with a hex dump: "[  12.345] TX: 81 2B F1 81 1E"
void log_hex(const char *prefix, const uint8_t *data, uint16_t len);

// ring buffer access for the /log.txt web endpoint
uint32_t log_total(void);                           // bytes ever written
String log_read_since(uint32_t from);               // text written after position 'from' (what is still in the buffer)
void log_tail(char *buf, size_t n);                 // last n-1 bytes of the log, terminated

#endif /* _LOG_H */
