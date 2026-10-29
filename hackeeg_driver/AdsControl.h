#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// HackEEG register and analog-front-end commands shared by USB text mode
// and the |command args| extension on WiFi and Bluetooth.

void adsControlSetSource(int source);
void adsControlSetStreamHook(void (*hook)(int source, bool on));

int adsDecodeId(uint8_t id, const char **name);
void adsControlApplyDefaults();
void adsNoteRegisterWrite(int reg, int val);

int adsControlExec(const char *line, char *text, size_t textLen);

uint8_t adsControlPowerMask();
int adsControlSampleRateHz();
int adsControlGainMultiplier(int channel);
bool adsControlBoardConnected();
bool adsControlApplyChannel(int channel, int powerDown, int gainCode, int input, int bias, int srb2, int srb1, char *err, size_t errLen);
bool adsControlSetPower(int channel, bool on, char *err, size_t errLen);
bool adsControlSetTestMode(const char *mode, char *err, size_t errLen);
bool adsControlSetLeadOffChannel(int channel, int pOn, int nOn, char *err, size_t errLen);
bool adsControlSetSampleRate(int hz, char *err, size_t errLen);
void adsControlAppendRegisters(Print &out);
bool adsControlReadFrame(uint8_t *raw, int nbytes, char *err, size_t errLen);
