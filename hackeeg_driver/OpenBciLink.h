#pragma once

#include <Arduino.h>

#if defined(ESP32)

void openBciSetup();
void openBciLoop();

#else

inline void openBciSetup() {}
inline void openBciLoop() {}

#endif
