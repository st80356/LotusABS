#pragma once
#include "Arduino.h"
enum { WIFI_AP = 2, WIFI_STA = 1 }; enum { WL_CONNECTED = 3 };
struct WiFiClass { void mode(int){} bool softAPConfig(IPAddress,IPAddress,IPAddress){return true;} bool softAP(const char*,const char*){return true;} IPAddress localIP(){return IPAddress(192,168,1,1);} int status(){return WL_CONNECTED;} void begin(const char*,const char*){} };
extern WiFiClass WiFi;
