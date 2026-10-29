#pragma once

// Board pins, USB serial port, and compile-time overrides.
// Override any HACKEEG_PIN_* or HACKEEG_CHANNEL_COUNT with a build flag,
// for example -DHACKEEG_CHANNEL_COUNT=6 or -DHACKEEG_PIN_DRDY=34.
//
// wifi_secrets.h is included first so a local SSID can replace the defaults.

#include "wifi_secrets.h"

#if defined(ESP32)

#ifndef HACKEEG_PIN_SCK
#define HACKEEG_PIN_SCK 18
#endif
#ifndef HACKEEG_PIN_MOSI
#define HACKEEG_PIN_MOSI 23
#endif
#ifndef HACKEEG_PIN_MISO
#define HACKEEG_PIN_MISO 19
#endif
#ifndef HACKEEG_PIN_CS
#define HACKEEG_PIN_CS 5
#endif
#ifndef HACKEEG_PIN_DRDY
#define HACKEEG_PIN_DRDY 27
#endif
#ifndef HACKEEG_PIN_START
#define HACKEEG_PIN_START 25
#endif
#ifndef HACKEEG_PIN_RESET
#define HACKEEG_PIN_RESET 26
#endif
#ifndef HACKEEG_PIN_PWDN
#define HACKEEG_PIN_PWDN 32
#endif
#ifndef HACKEEG_PIN_CLKSEL
#define HACKEEG_PIN_CLKSEL -1
#endif
#ifndef HACKEEG_PIN_LED
#define HACKEEG_PIN_LED 2
#endif
#define HACKEEG_SERIAL Serial

#elif defined(TEENSYDUINO)

// Same control pins as the stacked ADS1299 carrier used by the Teensy OpenBCI port.
// SCK/MOSI/MISO stay on the Teensy hardware SPI pins (13/11/12).
#ifndef HACKEEG_PIN_CS
#define HACKEEG_PIN_CS 10
#endif
#ifndef HACKEEG_PIN_DRDY
#define HACKEEG_PIN_DRDY 2
#endif
#ifndef HACKEEG_PIN_START
#define HACKEEG_PIN_START 6
#endif
#ifndef HACKEEG_PIN_RESET
#define HACKEEG_PIN_RESET 5
#endif
#ifndef HACKEEG_PIN_PWDN
#define HACKEEG_PIN_PWDN 3
#endif
#ifndef HACKEEG_PIN_CLKSEL
#define HACKEEG_PIN_CLKSEL -1
#endif
#ifndef HACKEEG_PIN_LED
#define HACKEEG_PIN_LED LED_BUILTIN
#endif
#define HACKEEG_SERIAL Serial

#else

// Arduino Due + HackEEG shield v1.5.0.
#ifndef HACKEEG_PIN_PWDN
#define HACKEEG_PIN_PWDN 33
#endif
#ifndef HACKEEG_PIN_CLKSEL
#define HACKEEG_PIN_CLKSEL 48
#endif
#ifndef HACKEEG_PIN_RESET
#define HACKEEG_PIN_RESET 47
#endif
#ifndef HACKEEG_PIN_START
#define HACKEEG_PIN_START 59
#endif
#ifndef HACKEEG_PIN_DRDY
#define HACKEEG_PIN_DRDY 24
#endif
#ifndef HACKEEG_PIN_CS
#define HACKEEG_PIN_CS 23
#endif
#ifndef HACKEEG_PIN_LED
#define HACKEEG_PIN_LED 13
#endif
#define HACKEEG_SERIAL SerialUSB

#endif

// 0 = use the ADS1299 ID register. On ESP32 an unreadable ID still frames 6 channels.
#ifndef HACKEEG_CHANNEL_COUNT
#define HACKEEG_CHANNEL_COUNT 0
#endif

#ifndef HACKEEG_BT_NAME
#define HACKEEG_BT_NAME "HackEEG"
#endif
#ifndef HACKEEG_AP_SSID
#define HACKEEG_AP_SSID "HackEEG"
#endif
#ifndef HACKEEG_AP_PASSWORD
#define HACKEEG_AP_PASSWORD "openbci123"
#endif
#ifndef HACKEEG_STA_SSID
#define HACKEEG_STA_SSID ""
#endif
#ifndef HACKEEG_STA_PASSWORD
#define HACKEEG_STA_PASSWORD ""
#endif

#define HACKEEG_FIRMWARE_VERSION "v0.4.0"

#ifndef PIN_LED
#define PIN_LED HACKEEG_PIN_LED
#endif
