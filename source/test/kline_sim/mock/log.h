#pragma once
#include <stdint.h>
void log_msg(const char *fmt, ...);
void log_line(const char *fmt, ...);
void log_hex(const char *prefix, const uint8_t *data, uint16_t len);
