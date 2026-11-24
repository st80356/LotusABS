#pragma once
#include "Arduino.h"
class Ticker { public: void attach_ms(uint32_t ms, void (*cb)()); };
