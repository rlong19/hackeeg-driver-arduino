#pragma once

#include <Arduino.h>

#define TEXT_MODE 0
#define JSONLINES_MODE 1
#define MESSAGEPACK_MODE 2

enum HackeegStreamSource {
    HACKEEG_STREAM_USB = 0,
    HACKEEG_STREAM_WIFI = 1,
    HACKEEG_STREAM_SPP = 2,
    HACKEEG_STREAM_HTTP = 3,
    HACKEEG_STREAM_TCP = 4
};

extern int protocol_mode;
extern int max_channels;
extern int num_spi_bytes;
extern int num_timestamped_spi_bytes;
extern const char *hardware_type;
extern bool id_valid;
extern uint8_t ads_id;
extern bool channels_forced;
extern boolean is_rdatac;
extern boolean base64_mode;

void send_response(int status_code, const char *status_text);
void detectActiveChannels();
void hackeegArmStream(int source, bool on);
void hackeegRebootAds();
void refreshAcquisition();
bool hackeegWifiStreaming();
bool hackeegSppStreaming();
void hackeegEmitUsbSample(uint32_t timestamp, uint32_t sampleNumber, const uint8_t *raw, uint8_t rawLen, bool force = false);
void hackeegAddExtensionCommands();
