#pragma once

#include <Arduino.h>

// One captured ADS1299 frame plus the 33-byte OpenBCI packet built from it.
struct HackeegSample {
    uint32_t timestamp;
    uint32_t sampleNumber;
    uint8_t rawLen;
    uint8_t raw[27];
    uint8_t packet[33];
};

#if defined(ESP32)

void sampleEngineBegin();
void sampleEngineAttach();
void sampleEngineSetCapture(bool on);
void sampleEngineResetCounter();
void sampleEngineClearNotify();
void sampleEngineDiscard();
bool sampleEnginePop(HackeegSample *out);
uint32_t sampleEngineRingDropped();
uint32_t sampleEngineDrdyMissed();
uint32_t sampleEngineRadioDropped();
uint32_t sampleEngineProduced();
void sampleEngineAddRadioDropped(uint32_t count);

#else

inline void sampleEngineBegin() {}
inline void sampleEngineAttach() {}
inline void sampleEngineSetCapture(bool) {}
inline void sampleEngineResetCounter() {}
inline void sampleEngineClearNotify() {}
inline void sampleEngineDiscard() {}
inline bool sampleEnginePop(HackeegSample *) { return false; }
inline uint32_t sampleEngineRingDropped() { return 0; }
inline uint32_t sampleEngineDrdyMissed() { return 0; }
inline uint32_t sampleEngineRadioDropped() { return 0; }
inline uint32_t sampleEngineProduced() { return 0; }
inline void sampleEngineAddRadioDropped(uint32_t) {}

#endif
