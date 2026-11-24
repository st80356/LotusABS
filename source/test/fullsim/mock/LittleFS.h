#pragma once
#include "Arduino.h"
struct LittleFSClass { bool begin() { return true; } };
extern LittleFSClass LittleFS;
