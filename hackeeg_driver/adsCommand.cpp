/**
 * send and receive commands from TI ADS129x chips.
 *
 * Copyright (c) 2013 by Adam Feuer <adam@adamfeuer.com>
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

#include "Arduino.h"
#include "adsCommand.h"
#include "ads129x.h"
#include "AdsControl.h"
#include "SpiDma.h"

#if defined(ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t g_spi_mutex = nullptr;
static volatile bool g_acquiring = false;
#else
static bool g_acquiring = false;
#endif

void adsBusInit() {
#if defined(ESP32)
    if (g_spi_mutex == nullptr) {
        g_spi_mutex = xSemaphoreCreateRecursiveMutex();
    }
#endif
}

void adsBusLock() {
#if defined(ESP32)
    if (g_spi_mutex == nullptr) {
        adsBusInit();
    }
    if (g_spi_mutex != nullptr) {
        xSemaphoreTakeRecursive(g_spi_mutex, portMAX_DELAY);
    }
#endif
}

void adsBusUnlock() {
#if defined(ESP32)
    if (g_spi_mutex != nullptr) {
        xSemaphoreGiveRecursive(g_spi_mutex);
    }
#endif
}

void adsSetAcquiring(bool on) {
    g_acquiring = on;
}

bool adsAcquiring() {
    return g_acquiring;
}

static void csLow() {
    digitalWrite(PIN_CS, LOW);
#if defined(ESP32)
    delayMicroseconds(4);
#endif
}

static void csHigh() {
    digitalWrite(PIN_CS, HIGH);
#if defined(ESP32)
    delayMicroseconds(4);
#endif
}

/*void wait_for_drdy(int interval)
{
	int i = 0;
	while (digitalRead(IPIN_DRDY) == HIGH) {
		if (i < interval) {
			continue;
		}
		i = 0;
	}
}*/

void adcSendCommand(int cmd) {
    adsBusLock();
    csLow();
    spiSend((uint8_t) cmd);
    delayMicroseconds(1);
    csHigh();
    adsBusUnlock();
}

void adcSendCommandLeaveCsActive(int cmd) {
    adsBusLock();
    csLow();
    spiSend((uint8_t) cmd);
    adsBusUnlock();
}

static void resumeContinuous() {
#if defined(ESP32)
    if (!g_acquiring) {
        return;
    }
    delayMicroseconds(4);
    csLow();
    spiSend(ADS129x::RDATAC);
    delayMicroseconds(2);
    csHigh();
#endif
}

static void pauseContinuous() {
#if defined(ESP32)
    if (!g_acquiring) {
        return;
    }
    csLow();
    spiSend(ADS129x::SDATAC);
    delayMicroseconds(2);
    csHigh();
    delayMicroseconds(4);
#endif
}

void adcWreg(int reg, int val) {
    adsBusLock();
    pauseContinuous();
    csLow();
    spiSend((uint8_t) (ADS129x::WREG | reg));
    delayMicroseconds(2);
    spiSend(0);
    delayMicroseconds(2);
    spiSend((uint8_t) val);
    delayMicroseconds(1);
    csHigh();
    adsNoteRegisterWrite(reg, val);
    resumeContinuous();
    adsBusUnlock();
}

int adcRreg(int reg) {
    adsBusLock();
    pauseContinuous();
    csLow();
    spiSend((uint8_t) (ADS129x::RREG | reg));
    delayMicroseconds(2);
    spiSend(0);
    delayMicroseconds(2);
    uint8_t out = spiRec();
    delayMicroseconds(1);
    csHigh();
    resumeContinuous();
    adsBusUnlock();
    return (int) out;
}
