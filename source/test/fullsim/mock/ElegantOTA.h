#pragma once
#include "ESP8266WebServer.h"
struct ElegantOtaClass { void begin(ESP8266WebServer*, const char* = "", const char* = "") {} };
extern ElegantOtaClass ElegantOTA;
struct UpdaterClass { std::function<void(size_t,size_t)> cb; UpdaterClass &onProgress(std::function<void(size_t,size_t)> f) { cb = f; return *this; } };
extern UpdaterClass Update;
