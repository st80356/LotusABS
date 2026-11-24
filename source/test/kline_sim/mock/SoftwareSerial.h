#pragma once
#include "Arduino.h"
class SoftwareSerial {
public:
  SoftwareSerial(int rx, int tx) {}
  void begin(uint32_t) { bus_port_open(true); }
  void end() { bus_port_open(false); }
  int available() { return bus_available(); }
  int read() { return bus_read(); }
  size_t write(uint8_t b) { bus_write(&b, 1); return 1; }
};
