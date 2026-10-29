/*
 * adsCommand.h
 *
 * Copyright (c) 2013-2019 by Adam Feuer <adam@adamfeuer.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _ADS_COMMAND_H
#define _ADS_COMMAND_H

#include "Arduino.h"
#include "BoardConfig.h"

// Pin numbers come from BoardConfig.h so the same sketch builds for the
// HackEEG Due shield, a Teensy carrier, and an original ESP32.
// Due defaults match HackEEG Shield v1.5.0 (board 0).

const int IPIN_PWDN = HACKEEG_PIN_PWDN;
const int PIN_CLKSEL = HACKEEG_PIN_CLKSEL;
const int IPIN_RESET = HACKEEG_PIN_RESET;
const int PIN_START = HACKEEG_PIN_START;
const int IPIN_DRDY = HACKEEG_PIN_DRDY;
const int PIN_CS = HACKEEG_PIN_CS;

void adcWreg(int reg, int val);
void adcSendCommand(int cmd);
void adcSendCommandLeaveCsActive(int cmd);
int adcRreg(int reg);
void adsBusInit();
void adsBusLock();
void adsBusUnlock();
void adsSetAcquiring(bool on);
bool adsAcquiring();

#endif // _ADS_COMMAND_H
