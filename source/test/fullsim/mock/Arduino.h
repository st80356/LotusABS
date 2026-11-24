#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>
#include <functional>
#include <cmath>
typedef uint8_t byte; typedef bool boolean;
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
#define SERIAL_8N1 0
uint32_t millis();
void delay(uint32_t ms);
void yield();
void pinMode(int, int);
void digitalWrite(int pin, int v);
int  bus_available(); int bus_read(); void bus_write(const uint8_t *d, size_t n); void bus_port_open(bool open);

class String {
  std::string s;
  static std::string num(long v, int base=10){ char b[40]; if(base==16) snprintf(b,40,"%lx",v); else snprintf(b,40,"%ld",v); return b; }
public:
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  String(char c) : s(1, c) {}
  String(unsigned char v, unsigned char base=10) : s(num(v, base)) {}
  String(int v, unsigned char base=10) : s(num(v, base)) {}
  String(unsigned int v, unsigned char base=10) : s(num((long)v, base)) {}
  String(long v, unsigned char base=10) : s(num(v, base)) {}
  String(unsigned long v, unsigned char base=10) : s(num((long)v, base)) {}
  String(float v, unsigned char dec=2) { char b[40]; snprintf(b,40,"%.*f",dec,(double)v); s=b; }
  String(double v, unsigned char dec=2) { char b[40]; snprintf(b,40,"%.*f",dec,v); s=b; }
  const char *c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  bool concat(const char *c) { s += c; return true; }
  bool concat(const String &o) { s += o.s; return true; }
  bool reserve(unsigned int n) { s.reserve(n); return true; }
  long toInt() const { return atol(s.c_str()); }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *c) { s += c; return *this; }
  String &operator+=(char c) { s += c; return *this; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator==(const char *c) const { return s == c; }
  bool operator!=(const String &o) const { return s != o.s; }
  char operator[](unsigned int i) const { return s[i]; }
  const std::string &std() const { return s; }
  friend String operator+(const String &a, const String &b) { return String(a.s + b.s); }
  friend String operator+(const String &a, const char *b) { return String(a.s + b); }
  friend String operator+(const char *a, const String &b) { return String(std::string(a) + b.s); }
};
extern const String emptyString;

class HardwareSerial {
public:
  void begin(uint32_t, int=0) { bus_port_open(true); }
  void end() { bus_port_open(false); }
  void flush() {}
  int available() { return bus_available(); }
  int read() { return bus_read(); }
  size_t write(const uint8_t *d, size_t n) { bus_write(d, n); return n; }
  size_t write(uint8_t b) { bus_write(&b, 1); return 1; }
  size_t print(const char *) { return 0; }
  size_t print(const String &) { return 0; }
  int printf(const char *, ...) { return 0; }
  void setDebugOutput(bool) {}
};
extern HardwareSerial Serial;

class EspClass {
public:
  uint32_t getFreeHeap();
  uint8_t getHeapFragmentation() { return 5; }
  String getResetReason();
  bool rtcUserMemoryRead(uint32_t offset, uint32_t *data, size_t size);
  bool rtcUserMemoryWrite(uint32_t offset, uint32_t *data, size_t size);
  [[noreturn]] void reset();
  [[noreturn]] void restart();
};
extern EspClass ESP;

class IPAddress { uint8_t a[4]; public: IPAddress(){memset(a,0,4);} IPAddress(uint8_t x,uint8_t y,uint8_t z,uint8_t w){a[0]=x;a[1]=y;a[2]=z;a[3]=w;} uint8_t operator[](int i) const { return a[i]; } };
