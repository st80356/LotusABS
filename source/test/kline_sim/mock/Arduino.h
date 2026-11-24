#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
#define SERIAL_8N1 0
extern uint32_t g_now;
inline uint32_t millis() { return g_now; }
void delay(uint32_t ms);
void yield();
void pinMode(int, int);
void digitalWrite(int pin, int v);
// the K-line as seen from the D1 mini (shared by both serial mocks)
int  bus_available();
int  bus_read();
void bus_write(const uint8_t *d, size_t n);
void bus_port_open(bool open);
class HardwareSerial {
public:
  void begin(uint32_t, int=0) { bus_port_open(true); }
  void end() { bus_port_open(false); }
  void flush() {}
  int available() { return bus_available(); }
  int read() { return bus_read(); }
  size_t write(const uint8_t *d, size_t n) { bus_write(d, n); return n; }
  size_t write(uint8_t b) { bus_write(&b, 1); return 1; }
};
extern HardwareSerial Serial;
