#pragma once
#include "Arduino.h"
#include <vector>
class Task { public: Task(unsigned long iv, long, void (*cb)()) : iv(iv), cb(cb) {} void enable(){ en=true; last=millis(); } unsigned long iv; void (*cb)(); bool en=false; uint32_t last=0; };
class Scheduler { std::vector<Task*> t; public: void init(){} void addTask(Task &x){ t.push_back(&x);} void execute(){ for(auto *x: t) if(x->en && millis()-x->last>=x->iv){ x->last=millis(); x->cb(); } } };
