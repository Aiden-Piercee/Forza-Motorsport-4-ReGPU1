#include "live_switch.h"
#include <cstdlib>
regpu::LiveSwitch::LiveSwitch(const char* name) : name_(name), value_(getenv(name) != nullptr) {}
