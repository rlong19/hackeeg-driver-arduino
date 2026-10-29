/*
 * ADS1299 register helpers and the HackEEG command set.
 *
 * USB text commands and the |command args| radio extension both land here.
 * OpenBCI single-character commands stay in OpenBciLink.cpp and call the
 * same channel, test-signal, lead-off, and sample-rate functions.
 */

#include "AdsControl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DriverState.h"
#include "SampleEngine.h"
#include "ads129x.h"
#include "adsCommand.h"
#include "SpiDma.h"

static const int REG_LOFF = 0x04;
static const uint8_t SRB2_BIT = 0x08;
static const uint8_t SRB1_BIT = 0x20;
static const uint8_t LOFF_COMP_EN = 0x02;
static const uint8_t CONFIG1_BASE = 0x90;
static const int kGainMultiplier[8] = {1, 2, 4, 6, 8, 12, 24, 24};

struct ChannelState {
    uint8_t pd;
    uint8_t gain;
    uint8_t input;
    uint8_t bias;
    uint8_t srb2;
    uint8_t srb1;
};

static ChannelState g_live[8];
static volatile uint8_t g_power_mask = 0;
static volatile int g_sample_hz = 250;
static int g_source = HACKEEG_STREAM_USB;
static void (*g_stream_hook)(int source, bool on) = nullptr;

static const char *kHelp =
    "nop help version status micros serialnumber rreg wreg rdata rdatac sdatac "
    "start stop reset wakeup standby led boardled channels samplerate gain power input "
    "srb1 srb2 bias biasp biasn biasch loff loffp loffn loffch loff_current loff_freq "
    "testsignal testamp defaults dropped base64 hex text jsonlines messagepack regs";

void adsControlSetSource(int source) {
    g_source = source;
}

void adsControlSetStreamHook(void (*hook)(int source, bool on)) {
    g_stream_hook = hook;
}

uint8_t adsControlPowerMask() {
    return g_power_mask;
}

int adsControlSampleRateHz() {
    return g_sample_hz;
}

int adsControlGainMultiplier(int channel) {
    if (channel < 1 || channel > 8) {
        return 24;
    }
    return kGainMultiplier[g_live[channel - 1].gain & 7];
}

bool adsControlBoardConnected() {
    return id_valid || channels_forced;
}

int adsDecodeId(uint8_t id, const char **name) {
    const char *decoded = "unrecognized";
    int channels = 0;
    switch (id & 0x1F) {
        case 0x10:
            decoded = "ADS1294";
            channels = 4;
            break;
        case 0x11:
            decoded = "ADS1296";
            channels = 6;
            break;
        case 0x12:
            decoded = "ADS1298";
            channels = 8;
            break;
        case 0x1C:
            decoded = "ADS1299-4";
            channels = 4;
            break;
        case 0x1D:
            decoded = "ADS1299-6";
            channels = 6;
            break;
        case 0x1E:
            decoded = "ADS1299";
            channels = 8;
            break;
        default:
            if ((id & 0x1C) == 0x1C) {
                if ((id & 0x03) == 0) {
                    decoded = "ADS1299-4";
                    channels = 4;
                } else if ((id & 0x03) == 1) {
                    decoded = "ADS1299-6";
                    channels = 6;
                } else if ((id & 0x03) == 2) {
                    decoded = "ADS1299";
                    channels = 8;
                }
            }
            break;
    }
    if (name) {
        *name = decoded;
    }
    return channels;
}

static int rateFromDr(uint8_t dr) {
    switch (dr & 0x07) {
        case 0: return 16000;
        case 1: return 8000;
        case 2: return 4000;
        case 3: return 2000;
        case 4: return 1000;
        case 5: return 500;
        case 6: return 250;
        default: return 0;
    }
}

static uint8_t drFromRate(int hz) {
    switch (hz) {
        case 16000: return 0;
        case 8000: return 1;
        case 4000: return 2;
        case 2000: return 3;
        case 1000: return 4;
        case 500: return 5;
        case 250: return 6;
        default: return 0xFF;
    }
}

void adsNoteRegisterWrite(int reg, int val) {
    uint8_t v = (uint8_t) val;
    if (reg >= ADS129x::CH1SET && reg <= ADS129x::CH8SET) {
        int index = reg - ADS129x::CH1SET;
        g_live[index].pd = (v & 0x80) ? 1 : 0;
        g_live[index].gain = (uint8_t) ((v >> 4) & 7);
        g_live[index].srb2 = (v & SRB2_BIT) ? 1 : 0;
        g_live[index].input = (uint8_t) (v & 7);
        if (g_live[index].pd || index >= max_channels) {
            g_power_mask = (uint8_t) (g_power_mask & ~(1u << index));
        } else {
            g_power_mask = (uint8_t) (g_power_mask | (1u << index));
        }
    } else if (reg == ADS129x::RLD_SENSP) {
        for (int i = 0; i < 8; i++) {
            g_live[i].bias = (v & (1u << i)) ? 1 : 0;
        }
    } else if (reg == ADS129x::PACE) {
        uint8_t srb1 = (v & SRB1_BIT) ? 1 : 0;
        for (int i = 0; i < 8; i++) {
            g_live[i].srb1 = srb1;
        }
    } else if (reg == ADS129x::CONFIG1) {
        int rate = rateFromDr(v);
        if (rate > 0) {
            g_sample_hz = rate;
        }
    }
}

static void writeChannel(int index, const ChannelState &state) {
    uint8_t value = 0;
    if (state.pd) {
        value |= 0x80;
    }
    value |= (uint8_t) ((state.gain & 7) << 4);
    if (state.srb2) {
        value |= SRB2_BIT;
    }
    value |= (uint8_t) (state.input & 7);
    adcWreg(ADS129x::CH1SET + index, value);
}

static uint8_t presentMask() {
    if (max_channels <= 0) {
        return 0;
    }
    if (max_channels >= 8) {
        return 0xFF;
    }
    return (uint8_t) ((1u << max_channels) - 1);
}

void adsControlApplyDefaults() {
    bool usb = is_rdatac;
    bool wifi = hackeegWifiStreaming();
    bool spp = hackeegSppStreaming();
    if (usb) {
        hackeegArmStream(HACKEEG_STREAM_USB, false);
    }
    if (wifi) {
        hackeegArmStream(HACKEEG_STREAM_WIFI, false);
    }
    if (spp) {
        hackeegArmStream(HACKEEG_STREAM_SPP, false);
    }

    ChannelState templ;
    templ.pd = 0;
    templ.gain = 6;
    templ.input = 0;
    templ.bias = 1;
    templ.srb2 = 0;
    templ.srb1 = 1;

    adcWreg(ADS129x::CONFIG1, CONFIG1_BASE | 0x06);
    adcWreg(ADS129x::CONFIG2, ADS129x::CONFIG2_const);
    uint8_t config3 = (uint8_t) (ADS129x::PD_REFBUF | ADS129x::CONFIG3_const | ADS129x::RLDREF_INT | ADS129x::PD_RLD);
    adcWreg(ADS129x::CONFIG3, config3);
    adcWreg(REG_LOFF, 0x00);
    adcWreg(ADS129x::CONFIG4, 0x00);
    adcWreg(ADS129x::PACE, SRB1_BIT);

    uint8_t mask = presentMask();
    adcWreg(ADS129x::RLD_SENSP, mask);
    adcWreg(ADS129x::RLD_SENSN, mask);
    adcWreg(ADS129x::LOFF_SENSP, 0x00);
    adcWreg(ADS129x::LOFF_SENSN, 0x00);
    adcWreg(ADS129x::LOFF_FLIP, 0x00);

    g_power_mask = 0;
    for (int i = 0; i < 8; i++) {
        if (i < max_channels) {
            g_live[i] = templ;
            writeChannel(i, g_live[i]);
        } else {
            g_live[i] = templ;
            g_live[i].pd = 1;
            g_live[i].bias = 0;
        }
    }
    g_sample_hz = 250;
    delay(150);

    if (usb) {
        hackeegArmStream(HACKEEG_STREAM_USB, true);
    }
    if (wifi) {
        hackeegArmStream(HACKEEG_STREAM_WIFI, true);
    }
    if (spp) {
        hackeegArmStream(HACKEEG_STREAM_SPP, true);
    }
}

static void copyLower(const char *line, char *buf, size_t len) {
    size_t n = 0;
    while (line && *line && n + 1 < len) {
        buf[n++] = (char) tolower((unsigned char) *line++);
    }
    buf[n] = 0;
}

static char *trimInPlace(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == '|') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '|')) {
        s[--n] = 0;
    }
    return s;
}

static bool parseHexByte(const char *s, long *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = nullptr;
    long value = strtol(s, &end, 16);
    if (end == s || *end != 0 || value < 0 || value > 255) {
        return false;
    }
    *out = value;
    return true;
}

static bool parseOnOff(const char *s, int *on) {
    if (!s) {
        return false;
    }
    if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
        *on = 1;
        return true;
    }
    if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
        *on = 0;
        return true;
    }
    return false;
}

static bool channelIndex(const char *s, int *channel, char *err, size_t errLen) {
    if (!s || !isdigit((unsigned char) s[0])) {
        snprintf(err, errLen, "channel required");
        return false;
    }
    int value = atoi(s);
    if (value < 1 || value > 8) {
        snprintf(err, errLen, "channel out of range");
        return false;
    }
    if (value > max_channels) {
        snprintf(err, errLen, "channel not present");
        return false;
    }
    *channel = value;
    return true;
}

static void arm(bool on) {
    if (g_stream_hook) {
        g_stream_hook(g_source, on);
    }
}

static int say(char *text, size_t textLen, int code, const char *message) {
    snprintf(text, textLen, "%s", message);
    return code;
}

static int cmdNop(int, char **, char *text, size_t textLen) {
    return say(text, textLen, 200, "Ok");
}

static int cmdHelp(int, char **, char *text, size_t textLen) {
    return say(text, textLen, 200, kHelp);
}

static int cmdVersion(int, char **, char *text, size_t textLen) {
    return say(text, textLen, 200, HACKEEG_FIRMWARE_VERSION);
}

static int cmdStatus(int, char **, char *text, size_t textLen) {
    snprintf(text, textLen,
             "id=0x%02X valid=%d type=%s channels=%d rate=%d streaming=%d ring_dropped=%lu drdy_missed=%lu radio_dropped=%lu produced=%lu",
             ads_id,
             id_valid ? 1 : 0,
             hardware_type ? hardware_type : "unknown",
             max_channels,
             g_sample_hz,
             adsAcquiring() ? 1 : 0,
             (unsigned long) sampleEngineRingDropped(),
             (unsigned long) sampleEngineDrdyMissed(),
             (unsigned long) sampleEngineRadioDropped(),
             (unsigned long) sampleEngineProduced());
    return 200;
}

static int cmdMicros(int, char **, char *text, size_t textLen) {
    snprintf(text, textLen, "%lu", (unsigned long) micros());
    return 200;
}

static int cmdSerial(int, char **, char *text, size_t textLen) {
    return say(text, textLen, 501, "Not Implemented");
}

static int cmdRreg(int argc, char **argv, char *text, size_t textLen) {
    long reg = 0;
    if (argc < 2 || !parseHexByte(argv[1], &reg)) {
        return say(text, textLen, 400, "rreg expects a hex register");
    }
    int value = adcRreg((int) reg);
    snprintf(text, textLen, "%02X", value & 0xFF);
    return 200;
}

static int cmdWreg(int argc, char **argv, char *text, size_t textLen) {
    long reg = 0;
    long value = 0;
    if (argc < 3 || !parseHexByte(argv[1], &reg) || !parseHexByte(argv[2], &value)) {
        return say(text, textLen, 400, "wreg expects hex register and value");
    }
    adcWreg((int) reg, (int) value);
    snprintf(text, textLen, "Ok %02X %02X", (unsigned) reg, (unsigned) value);
    return 200;
}

static int cmdRdata(int, char **, char *text, size_t textLen) {
    uint8_t raw[27];
    int n = num_spi_bytes;
    if (n < 3) {
        n = 3;
    }
    if (n > (int) sizeof(raw)) {
        n = (int) sizeof(raw);
    }
    char err[64];
    if (!adsControlReadFrame(raw, n, err, sizeof(err))) {
        return say(text, textLen, 400, err);
    }
    size_t used = 0;
    for (int i = 0; i < n && used + 2 < textLen; i++) {
        used += (size_t) snprintf(text + used, textLen - used, "%02X", raw[i]);
    }
    return 200;
}

static int cmdRdatac(int, char **, char *text, size_t textLen) {
    arm(true);
    return say(text, textLen, 200, "Ok");
}

static int cmdSdatac(int, char **, char *text, size_t textLen) {
    arm(false);
    return say(text, textLen, 200, "Ok");
}

static int cmdStart(int, char **, char *text, size_t textLen) {
    adcSendCommand(ADS129x::START);
    return say(text, textLen, 200, "Ok");
}

static int cmdStop(int, char **, char *text, size_t textLen) {
    adcSendCommand(ADS129x::STOP);
    return say(text, textLen, 200, "Ok");
}

static int cmdReset(int, char **, char *text, size_t textLen) {
    hackeegRebootAds();
    return say(text, textLen, 200, "Ok");
}

static int cmdWakeup(int, char **, char *text, size_t textLen) {
    adcSendCommand(ADS129x::WAKEUP);
    return say(text, textLen, 200, "Ok");
}

static int cmdStandby(int, char **, char *text, size_t textLen) {
    adcSendCommand(ADS129x::STANDBY);
    return say(text, textLen, 200, "Ok");
}

static int cmdLed(int argc, char **argv, char *text, size_t textLen) {
    int on = 0;
    if (argc < 2 || !parseOnOff(argv[1], &on)) {
        return say(text, textLen, 400, "led on|off");
    }
    digitalWrite(PIN_LED, on ? HIGH : LOW);
    return say(text, textLen, 200, "Ok");
}

static int cmdBoardLed(int argc, char **argv, char *text, size_t textLen) {
    int on = 0;
    if (argc < 2 || !parseOnOff(argv[1], &on)) {
        return say(text, textLen, 400, "boardled on|off");
    }
    int state = adcRreg(ADS129x::GPIO);
    if (on) {
        state = (state & 0xF7) | 0x80;
    } else {
        state = state & 0x77;
    }
    adcWreg(ADS129x::GPIO, state);
    return say(text, textLen, 200, "Ok");
}

static int cmdChannels(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 2) {
        snprintf(text, textLen, "%d", max_channels);
        return 200;
    }
    int n = atoi(argv[1]);
    if (n != 4 && n != 6 && n != 8) {
        return say(text, textLen, 400, "channels must be 4, 6, or 8");
    }
    bool usb = is_rdatac;
    bool wifi = hackeegWifiStreaming();
    bool spp = hackeegSppStreaming();
    if (usb) hackeegArmStream(HACKEEG_STREAM_USB, false);
    if (wifi) hackeegArmStream(HACKEEG_STREAM_WIFI, false);
    if (spp) hackeegArmStream(HACKEEG_STREAM_SPP, false);
    adsBusLock();
    max_channels = n;
    num_spi_bytes = 3 * (n + 1);
    num_timestamped_spi_bytes = num_spi_bytes + 8;
    channels_forced = true;
    g_power_mask = (uint8_t) (g_power_mask & presentMask());
    adsBusUnlock();
    if (usb) hackeegArmStream(HACKEEG_STREAM_USB, true);
    if (wifi) hackeegArmStream(HACKEEG_STREAM_WIFI, true);
    if (spp) hackeegArmStream(HACKEEG_STREAM_SPP, true);
    snprintf(text, textLen, "%d", n);
    return 200;
}

static int cmdSampleRate(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 2) {
        snprintf(text, textLen, "%d", g_sample_hz);
        return 200;
    }
    char err[64];
    if (!adsControlSetSampleRate(atoi(argv[1]), err, sizeof(err))) {
        return say(text, textLen, 400, err);
    }
    snprintf(text, textLen, "%d", g_sample_hz);
    return 200;
}

static int gainCodeFromMultiplier(int gain) {
    switch (gain) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 6: return 3;
        case 8: return 4;
        case 12: return 5;
        case 24: return 6;
        default: return -1;
    }
}

static int cmdGain(int argc, char **argv, char *text, size_t textLen) {
    char err[64];
    int channel = 0;
    if (argc < 3 || !channelIndex(argv[1], &channel, err, sizeof(err))) {
        return say(text, textLen, 400, argc < 3 ? "gain <ch> <1|2|4|6|8|12|24>" : err);
    }
    int code = gainCodeFromMultiplier(atoi(argv[2]));
    if (code < 0) {
        return say(text, textLen, 400, "gain must be 1, 2, 4, 6, 8, 12, or 24");
    }
    int index = channel - 1;
    g_live[index].gain = (uint8_t) code;
    writeChannel(index, g_live[index]);
    return say(text, textLen, 200, "Ok");
}

static int cmdPower(int argc, char **argv, char *text, size_t textLen) {
    char err[64];
    int channel = 0;
    int on = 0;
    if (argc < 3 || !channelIndex(argv[1], &channel, err, sizeof(err)) || !parseOnOff(argv[2], &on)) {
        return say(text, textLen, 400, "power <ch> on|off");
    }
    if (!adsControlSetPower(channel, on != 0, err, sizeof(err))) {
        return say(text, textLen, 400, err);
    }
    return say(text, textLen, 200, "Ok");
}

static int inputCode(const char *s) {
    if (!strcmp(s, "normal") || !strcmp(s, "0")) return 0;
    if (!strcmp(s, "shorted") || !strcmp(s, "short") || !strcmp(s, "1")) return 1;
    if (!strcmp(s, "bias") || !strcmp(s, "2")) return 2;
    if (!strcmp(s, "mvdd") || !strcmp(s, "3")) return 3;
    if (!strcmp(s, "temp") || !strcmp(s, "4")) return 4;
    if (!strcmp(s, "test") || !strcmp(s, "5")) return 5;
    if (!strcmp(s, "bias_drp") || !strcmp(s, "6")) return 6;
    if (!strcmp(s, "bias_drn") || !strcmp(s, "7")) return 7;
    return -1;
}

static int cmdInput(int argc, char **argv, char *text, size_t textLen) {
    char err[64];
    int channel = 0;
    if (argc < 3 || !channelIndex(argv[1], &channel, err, sizeof(err))) {
        return say(text, textLen, 400, "input <ch> normal|shorted|bias|mvdd|temp|test|bias_drp|bias_drn");
    }
    int code = inputCode(argv[2]);
    if (code < 0) {
        return say(text, textLen, 400, "unknown input");
    }
    int index = channel - 1;
    g_live[index].input = (uint8_t) code;
    writeChannel(index, g_live[index]);
    return say(text, textLen, 200, "Ok");
}

static int cmdSrb1(int argc, char **argv, char *text, size_t textLen) {
    int on = 0;
    if (argc < 2 || !parseOnOff(argv[1], &on)) {
        return say(text, textLen, 400, "srb1 on|off");
    }
    int misc = adcRreg(ADS129x::PACE);
    if (on) {
        misc |= SRB1_BIT;
    } else {
        misc &= ~SRB1_BIT;
    }
    adcWreg(ADS129x::PACE, misc);
    return say(text, textLen, 200, "Ok");
}

static int cmdSrb2(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 3) {
        return say(text, textLen, 400, "srb2 <ch|all> on|off");
    }
    int on = 0;
    if (!parseOnOff(argv[2], &on) && !(argc >= 3 && parseOnOff(argv[argc - 1], &on))) {
        return say(text, textLen, 400, "srb2 <ch|all> on|off");
    }
    if (strcmp(argv[1], "all") == 0) {
        for (int i = 0; i < max_channels; i++) {
            g_live[i].srb2 = (uint8_t) on;
            writeChannel(i, g_live[i]);
        }
        return say(text, textLen, 200, "Ok");
    }
    char err[64];
    int channel = 0;
    if (!channelIndex(argv[1], &channel, err, sizeof(err))) {
        return say(text, textLen, 400, err);
    }
    g_live[channel - 1].srb2 = (uint8_t) on;
    writeChannel(channel - 1, g_live[channel - 1]);
    return say(text, textLen, 200, "Ok");
}

static int cmdBias(int argc, char **argv, char *text, size_t textLen) {
    int on = 0;
    if (argc < 2 || !parseOnOff(argv[1], &on)) {
        return say(text, textLen, 400, "bias on|off");
    }
    int config3 = adcRreg(ADS129x::CONFIG3);
    config3 |= ADS129x::PD_REFBUF | ADS129x::CONFIG3_const;
    if (on) {
        config3 |= ADS129x::RLDREF_INT | ADS129x::PD_RLD;
    } else {
        config3 &= ~(ADS129x::RLDREF_INT | ADS129x::PD_RLD);
    }
    adcWreg(ADS129x::CONFIG3, config3);
    return say(text, textLen, 200, "Ok");
}

static int writeMaskReg(int reg, int argc, char **argv, char *text, size_t textLen, const char *usage) {
    long mask = 0;
    if (argc < 2 || !parseHexByte(argv[1], &mask)) {
        return say(text, textLen, 400, usage);
    }
    adcWreg(reg, (int) mask);
    return say(text, textLen, 200, "Ok");
}

static int cmdBiasP(int argc, char **argv, char *text, size_t textLen) {
    return writeMaskReg(ADS129x::RLD_SENSP, argc, argv, text, textLen, "biasp <hex>");
}

static int cmdBiasN(int argc, char **argv, char *text, size_t textLen) {
    return writeMaskReg(ADS129x::RLD_SENSN, argc, argv, text, textLen, "biasn <hex>");
}

static int cmdBiasCh(int argc, char **argv, char *text, size_t textLen) {
    char err[64];
    int channel = 0;
    int on = 0;
    if (argc < 3 || !channelIndex(argv[1], &channel, err, sizeof(err)) || !parseOnOff(argv[2], &on)) {
        return say(text, textLen, 400, "biasch <ch> on|off");
    }
    uint8_t bit = (uint8_t) (1u << (channel - 1));
    int sp = adcRreg(ADS129x::RLD_SENSP);
    int sn = adcRreg(ADS129x::RLD_SENSN);
    if (on) {
        sp |= bit;
        sn |= bit;
    } else {
        sp &= ~bit;
        sn &= ~bit;
    }
    adcWreg(ADS129x::RLD_SENSP, sp);
    adcWreg(ADS129x::RLD_SENSN, sn);
    return say(text, textLen, 200, "Ok");
}

static int cmdLoff(int argc, char **argv, char *text, size_t textLen) {
    int on = 0;
    if (argc < 2 || !parseOnOff(argv[1], &on)) {
        return say(text, textLen, 400, "loff on|off");
    }
    int config4 = adcRreg(ADS129x::CONFIG4);
    if (on) {
        config4 |= LOFF_COMP_EN;
    } else {
        config4 &= ~LOFF_COMP_EN;
    }
    adcWreg(ADS129x::CONFIG4, config4);
    return say(text, textLen, 200, "Ok");
}

static int cmdLoffP(int argc, char **argv, char *text, size_t textLen) {
    return writeMaskReg(ADS129x::LOFF_SENSP, argc, argv, text, textLen, "loffp <hex>");
}

static int cmdLoffN(int argc, char **argv, char *text, size_t textLen) {
    return writeMaskReg(ADS129x::LOFF_SENSN, argc, argv, text, textLen, "loffn <hex>");
}

static int cmdLoffCh(int argc, char **argv, char *text, size_t textLen) {
    char err[64];
    int channel = 0;
    if (argc < 4 || !channelIndex(argv[1], &channel, err, sizeof(err))) {
        return say(text, textLen, 400, argc >= 4 ? err : "loffch <ch> <p 0|1> <n 0|1>");
    }
    int p = 0;
    int n = 0;
    if (!parseOnOff(argv[2], &p) || !parseOnOff(argv[3], &n)) {
        return say(text, textLen, 400, "loffch <ch> <p 0|1> <n 0|1>");
    }
    if (!adsControlSetLeadOffChannel(channel, p, n, err, sizeof(err))) {
        return say(text, textLen, 400, err);
    }
    return say(text, textLen, 200, "Ok");
}

static int cmdLoffCurrent(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 2) {
        return say(text, textLen, 400, "loff_current 6nA|24nA|6uA|24uA");
    }
    int code = -1;
    if (!strcmp(argv[1], "6na") || !strcmp(argv[1], "6")) code = 0;
    else if (!strcmp(argv[1], "24na") || !strcmp(argv[1], "24")) code = 1;
    else if (!strcmp(argv[1], "6ua")) code = 2;
    else if (!strcmp(argv[1], "24ua")) code = 3;
    if (code < 0) {
        return say(text, textLen, 400, "ADS1299 lead-off current is 6nA, 24nA, 6uA, or 24uA");
    }
    int loff = adcRreg(REG_LOFF);
    loff = (loff & ~0x0C) | (code << 2);
    adcWreg(REG_LOFF, loff);
    return say(text, textLen, 200, "Ok");
}

static int cmdLoffFreq(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 2) {
        return say(text, textLen, 400, "loff_freq dc|7.8|31.2|fdr4");
    }
    int code = -1;
    if (!strcmp(argv[1], "dc")) code = 0;
    else if (!strcmp(argv[1], "7.8") || !strcmp(argv[1], "7p8")) code = 1;
    else if (!strcmp(argv[1], "31.2") || !strcmp(argv[1], "31p2")) code = 2;
    else if (!strcmp(argv[1], "fdr4") || !strcmp(argv[1], "ac")) code = 3;
    if (code < 0) {
        return say(text, textLen, 400, "loff_freq dc|7.8|31.2|fdr4");
    }
    int loff = adcRreg(REG_LOFF);
    loff = (loff & ~0x03) | code;
    adcWreg(REG_LOFF, loff);
    return say(text, textLen, 200, "Ok");
}

static int cmdTestSignal(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 2) {
        return say(text, textLen, 400, "testsignal off|dc|slow|fast|slow2|fast2|ground");
    }
    char err[64];
    if (!adsControlSetTestMode(argv[1], err, sizeof(err))) {
        return say(text, textLen, 400, err);
    }
    return say(text, textLen, 200, "Ok");
}

static int cmdTestAmp(int argc, char **argv, char *text, size_t textLen) {
    if (argc < 2) {
        return say(text, textLen, 400, "testamp 1|2");
    }
    int amp = atoi(argv[1]);
    if (amp != 1 && amp != 2) {
        return say(text, textLen, 400, "testamp 1|2");
    }
    int config2 = adcRreg(ADS129x::CONFIG2);
    if (amp == 2) {
        config2 |= ADS129x::TEST_AMP;
    } else {
        config2 &= ~ADS129x::TEST_AMP;
    }
    adcWreg(ADS129x::CONFIG2, config2);
    return say(text, textLen, 200, "Ok");
}

static int cmdDefaults(int, char **, char *text, size_t textLen) {
    adsControlApplyDefaults();
    return say(text, textLen, 200, "Ok");
}

static int cmdDropped(int, char **, char *text, size_t textLen) {
    snprintf(text, textLen, "ring_dropped=%lu drdy_missed=%lu radio_dropped=%lu produced=%lu",
             (unsigned long) sampleEngineRingDropped(),
             (unsigned long) sampleEngineDrdyMissed(),
             (unsigned long) sampleEngineRadioDropped(),
             (unsigned long) sampleEngineProduced());
    return 200;
}

static int cmdProtocol(int, char **argv, char *text, size_t textLen) {
    if (!strcmp(argv[0], "text")) protocol_mode = TEXT_MODE;
    else if (!strcmp(argv[0], "jsonlines")) protocol_mode = JSONLINES_MODE;
    else if (!strcmp(argv[0], "messagepack")) protocol_mode = MESSAGEPACK_MODE;
    else if (!strcmp(argv[0], "base64")) base64_mode = true;
    else if (!strcmp(argv[0], "hex")) base64_mode = false;
    return say(text, textLen, 200, "Ok");
}

static int cmdRegs(int, char **, char *text, size_t textLen) {
    size_t used = 0;
    text[0] = 0;
    for (int reg = 0; reg <= 0x17; reg++) {
        int value = adcRreg(reg);
        int wrote = snprintf(text + used, textLen - used, "%s%02X=%02X", used ? " " : "", reg, value & 0xFF);
        if (wrote < 0 || (size_t) wrote >= textLen - used) {
            break;
        }
        used += (size_t) wrote;
    }
    return 200;
}

struct CommandEntry {
    const char *name;
    int (*fn)(int argc, char **argv, char *text, size_t textLen);
};

static const CommandEntry kCommands[] = {
    {"nop", cmdNop},
    {"help", cmdHelp},
    {"version", cmdVersion},
    {"status", cmdStatus},
    {"micros", cmdMicros},
    {"serialnumber", cmdSerial},
    {"rreg", cmdRreg},
    {"wreg", cmdWreg},
    {"rdata", cmdRdata},
    {"rdatac", cmdRdatac},
    {"sdatac", cmdSdatac},
    {"start", cmdStart},
    {"stop", cmdStop},
    {"reset", cmdReset},
    {"wakeup", cmdWakeup},
    {"standby", cmdStandby},
    {"led", cmdLed},
    {"boardled", cmdBoardLed},
    {"channels", cmdChannels},
    {"samplerate", cmdSampleRate},
    {"gain", cmdGain},
    {"power", cmdPower},
    {"input", cmdInput},
    {"srb1", cmdSrb1},
    {"srb2", cmdSrb2},
    {"bias", cmdBias},
    {"biasp", cmdBiasP},
    {"biasn", cmdBiasN},
    {"biasch", cmdBiasCh},
    {"loff", cmdLoff},
    {"loffp", cmdLoffP},
    {"loffn", cmdLoffN},
    {"loffch", cmdLoffCh},
    {"loff_current", cmdLoffCurrent},
    {"loff_freq", cmdLoffFreq},
    {"testsignal", cmdTestSignal},
    {"testamp", cmdTestAmp},
    {"defaults", cmdDefaults},
    {"dropped", cmdDropped},
    {"text", cmdProtocol},
    {"jsonlines", cmdProtocol},
    {"messagepack", cmdProtocol},
    {"base64", cmdProtocol},
    {"hex", cmdProtocol},
    {"regs", cmdRegs},
};

int adsControlExec(const char *line, char *text, size_t textLen) {
    if (!text || textLen == 0) {
        return 500;
    }
    text[0] = 0;
    char buf[128];
    copyLower(line, buf, sizeof(buf));
    char *cursor = trimInPlace(buf);
    char *argv[6];
    int argc = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(cursor, " \t", &save); tok && argc < 6; tok = strtok_r(nullptr, " \t", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return say(text, textLen, 400, "empty command");
    }
    for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        if (strcmp(argv[0], kCommands[i].name) == 0) {
            return kCommands[i].fn(argc, argv, text, textLen);
        }
    }
    return say(text, textLen, 406, "unknown command");
}

bool adsControlApplyChannel(int channel, int powerDown, int gainCode, int input, int bias, int srb2, int srb1, char *err, size_t errLen) {
    if (channel < 1 || channel > 8) {
        snprintf(err, errLen, "channel out of range");
        return false;
    }
    if (channel > max_channels) {
        snprintf(err, errLen, "channel not present");
        return false;
    }
    if (powerDown < 0 || powerDown > 1 || gainCode < 0 || gainCode > 6 || input < 0 || input > 7 ||
        bias < 0 || bias > 1 || srb2 < 0 || srb2 > 1 || srb1 < 0 || srb1 > 1) {
        snprintf(err, errLen, "bad channel setting");
        return false;
    }
    int index = channel - 1;
    g_live[index].pd = (uint8_t) powerDown;
    g_live[index].gain = (uint8_t) gainCode;
    g_live[index].input = (uint8_t) input;
    g_live[index].bias = (uint8_t) bias;
    g_live[index].srb2 = (uint8_t) srb2;
    g_live[index].srb1 = (uint8_t) srb1;
    writeChannel(index, g_live[index]);

    uint8_t bit = (uint8_t) (1u << index);
    int sp = adcRreg(ADS129x::RLD_SENSP);
    int sn = adcRreg(ADS129x::RLD_SENSN);
    if (bias) {
        sp |= bit;
        sn |= bit;
    } else {
        sp &= ~bit;
        sn &= ~bit;
    }
    adcWreg(ADS129x::RLD_SENSP, sp);
    adcWreg(ADS129x::RLD_SENSN, sn);

    int misc = adcRreg(ADS129x::PACE);
    if (srb1) {
        misc |= SRB1_BIT;
    } else {
        misc &= ~SRB1_BIT;
    }
    adcWreg(ADS129x::PACE, misc);
    return true;
}

bool adsControlSetPower(int channel, bool on, char *err, size_t errLen) {
    if (channel < 1 || channel > max_channels) {
        snprintf(err, errLen, "channel not present");
        return false;
    }
    int index = channel - 1;
    g_live[index].pd = on ? 0 : 1;
    if (!on) {
        g_live[index].input = ADS129x::SHORTED;
    } else if (g_live[index].input == ADS129x::SHORTED) {
        g_live[index].input = ADS129x::ELECTRODE_INPUT;
    }
    writeChannel(index, g_live[index]);
    return true;
}

static void setPresentMux(uint8_t mux) {
    for (int i = 0; i < max_channels; i++) {
        g_live[i].input = mux;
        g_live[i].pd = 0;
        writeChannel(i, g_live[i]);
    }
}

bool adsControlSetTestMode(const char *mode, char *err, size_t errLen) {
    if (!mode) {
        snprintf(err, errLen, "missing test mode");
        return false;
    }
    uint8_t config2 = ADS129x::CONFIG2_const;
    uint8_t mux = ADS129x::ELECTRODE_INPUT;
    if (!strcmp(mode, "off")) {
        config2 = ADS129x::CONFIG2_const;
        mux = ADS129x::ELECTRODE_INPUT;
    } else if (!strcmp(mode, "dc")) {
        config2 = (uint8_t) (ADS129x::CONFIG2_const | ADS129x::INT_TEST | ADS129x::TEST_FREQ1 | ADS129x::TEST_FREQ0);
        mux = ADS129x::TEST_SIGNAL;
    } else if (!strcmp(mode, "slow")) {
        config2 = (uint8_t) (ADS129x::CONFIG2_const | ADS129x::INT_TEST);
        mux = ADS129x::TEST_SIGNAL;
    } else if (!strcmp(mode, "fast")) {
        config2 = (uint8_t) (ADS129x::CONFIG2_const | ADS129x::INT_TEST | ADS129x::TEST_FREQ0);
        mux = ADS129x::TEST_SIGNAL;
    } else if (!strcmp(mode, "slow2")) {
        config2 = (uint8_t) (ADS129x::CONFIG2_const | ADS129x::INT_TEST | ADS129x::TEST_AMP);
        mux = ADS129x::TEST_SIGNAL;
    } else if (!strcmp(mode, "fast2")) {
        config2 = (uint8_t) (ADS129x::CONFIG2_const | ADS129x::INT_TEST | ADS129x::TEST_AMP | ADS129x::TEST_FREQ0);
        mux = ADS129x::TEST_SIGNAL;
    } else if (!strcmp(mode, "ground")) {
        config2 = ADS129x::CONFIG2_const;
        mux = ADS129x::SHORTED;
    } else {
        snprintf(err, errLen, "testsignal off|dc|slow|fast|slow2|fast2|ground");
        return false;
    }
    adcWreg(ADS129x::CONFIG2, config2);
    setPresentMux(mux);
    return true;
}

bool adsControlSetLeadOffChannel(int channel, int pOn, int nOn, char *err, size_t errLen) {
    if (channel < 1 || channel > max_channels) {
        snprintf(err, errLen, "channel not present");
        return false;
    }
    uint8_t bit = (uint8_t) (1u << (channel - 1));
    int sp = adcRreg(ADS129x::LOFF_SENSP);
    int sn = adcRreg(ADS129x::LOFF_SENSN);
    if (pOn) sp |= bit; else sp &= ~bit;
    if (nOn) sn |= bit; else sn &= ~bit;
    adcWreg(ADS129x::LOFF_SENSP, sp);
    adcWreg(ADS129x::LOFF_SENSN, sn);
    if (pOn || nOn) {
        int config4 = adcRreg(ADS129x::CONFIG4);
        adcWreg(ADS129x::CONFIG4, config4 | LOFF_COMP_EN);
    }
    return true;
}

bool adsControlSetSampleRate(int hz, char *err, size_t errLen) {
    uint8_t dr = drFromRate(hz);
    if (dr == 0xFF) {
        snprintf(err, errLen, "rate must be 250, 500, 1000, 2000, 4000, 8000, or 16000");
        return false;
    }
    int current = adcRreg(ADS129x::CONFIG1);
    uint8_t next = (uint8_t) ((current & 0xF8) | dr);
    if ((next & 0xF0) == 0) {
        next = (uint8_t) (CONFIG1_BASE | dr);
    }
    if ((next & 0x80) == 0 || (next & 0x10) == 0) {
        next = (uint8_t) (CONFIG1_BASE | dr);
    }
    adcWreg(ADS129x::CONFIG1, next);
    g_sample_hz = hz;
    return true;
}

void adsControlAppendRegisters(Print &out) {
    out.print("HackEEG ADS registers\n");
    for (int reg = 0; reg <= 0x17; reg++) {
        int value = adcRreg(reg);
        char line[24];
        snprintf(line, sizeof(line), "%02X 0x%02X\n", reg, value & 0xFF);
        out.print(line);
    }
    out.print("$$$\n");
}

bool adsControlReadFrame(uint8_t *raw, int nbytes, char *err, size_t errLen) {
    if (!raw || nbytes < 3) {
        snprintf(err, errLen, "bad frame");
        return false;
    }
    if (adsAcquiring()) {
        snprintf(err, errLen, "stop streaming first");
        return false;
    }
    bool opcodeStarted = false;
    if (digitalRead(PIN_START) == LOW) {
        adcSendCommand(ADS129x::START);
        opcodeStarted = true;
    }
    uint32_t start = millis();
    while (digitalRead(IPIN_DRDY) == HIGH) {
        if (millis() - start > 100) {
            if (opcodeStarted) {
                adcSendCommand(ADS129x::STOP);
            }
            snprintf(err, errLen, "DRDY timeout");
            return false;
        }
        delay(1);
    }
    if (nbytes > 27) {
        nbytes = 27;
    }
    adsBusLock();
    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(4);
    spiSend(ADS129x::RDATA);
    delayMicroseconds(4);
    spiRec(raw, (size_t) nbytes);
    digitalWrite(PIN_CS, HIGH);
    adsBusUnlock();
    if (opcodeStarted) {
        adcSendCommand(ADS129x::STOP);
    }
    return true;
}
