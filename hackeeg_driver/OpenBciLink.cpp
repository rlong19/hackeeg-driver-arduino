// OpenBCI Cyton transport for the original ESP32.
//
// Binary samples are 33 bytes: 0xA0, sample counter, eight 24-bit channels,
// six zero aux bytes, 0xC0. WiFi follows the OpenBCI WiFi Shield HTTP API
// (port 80) and then pushes those packets to a TCP or UDP socket on the
// computer. Classic Bluetooth SPP presents the same byte stream.
//
// HackEEG commands that do not fit in one Cyton character use a pipe frame:
//   |rreg 00|   |wreg 03 EC|   |srb1 on|   |loffch 1 1 1|
// The leading '|' is not a Cyton command, and bytes inside the frame are
// not interpreted as b/s/x/~/z. '{' and '}' are left for the Cyton WiFi
// attach commands and are ignored here.

#include "OpenBciLink.h"

#if defined(ESP32)

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "HackEEG WiFi/Bluetooth targets the original ESP32 (classic Bluetooth SPP)."
#endif
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Enable Bluetooth so classic SPP can build. Use the esp32 Dev Module board."
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <BluetoothSerial.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "AdsControl.h"
#include "BoardConfig.h"
#include "DriverState.h"
#include "SampleEngine.h"

#if defined(ESP32)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static BluetoothSerial g_bt;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

static WebServer g_server(80);
static WiFiUDP g_ssdp;
static WiFiUDP g_udp;
static WiFiClient g_tcp;

static bool g_sta = false;
static bool g_tcp_on = false;
static bool g_udp_on = false;
static bool g_json = false;
static bool g_delimiter = true;
static bool g_udp_repeat = false;
static int g_latency_us = 10000;
static IPAddress g_host;
static uint16_t g_port = 0;
static bool g_spp_was_up = false;
static char g_reply[1024];
static size_t g_reply_len = 0;

enum ParseState : uint8_t {
    ST_IDLE = 0,
    ST_PIPE,
    ST_CHANNEL,
    ST_LEADS,
    ST_RATE,
    ST_BOARD
};

static ParseState g_state = ST_IDLE;
static char g_acc[96];
static uint8_t g_acc_len = 0;
static uint32_t g_state_ms = 0;

static void replyClear() {
    g_reply_len = 0;
    g_reply[0] = 0;
}

static void replyAdd(const char *text) {
    if (!text) {
        return;
    }
    while (*text && g_reply_len + 1 < sizeof(g_reply)) {
        g_reply[g_reply_len++] = *text++;
    }
    g_reply[g_reply_len] = 0;
}

static bool transportStreaming(int source) {
    if (source == HACKEEG_STREAM_SPP) {
        return hackeegSppStreaming();
    }
    if (source == HACKEEG_STREAM_USB) {
        return false;
    }
    return hackeegWifiStreaming();
}

static void emit(Print *out, int source, const char *text) {
    replyAdd(text);
    if (!out || !text || text[0] == 0) {
        return;
    }
    if (transportStreaming(source)) {
        return;
    }
    out->print(text);
}

static int streamSource(int source) {
    if (source == HACKEEG_STREAM_SPP) {
        return HACKEEG_STREAM_SPP;
    }
    return HACKEEG_STREAM_WIFI;
}

static void enterState(ParseState state) {
    g_state = state;
    g_acc_len = 0;
    g_state_ms = millis();
}

class ReplyPrint : public Print {
public:
    size_t write(uint8_t c) override {
        char text[2] = {(char) c, 0};
        replyAdd(text);
        return 1;
    }
    size_t write(const uint8_t *buffer, size_t size) override {
        for (size_t i = 0; i < size; i++) {
            write(buffer[i]);
        }
        return size;
    }
};

static void writeBanner(Print &out) {
    char line[96];
    out.print("OpenBCI V3 8-16 channel\n");
    snprintf(line, sizeof(line), "ADS1299 Device ID: 0x%02X\n", ads_id);
    out.print(line);
    out.print("LIS3DH Device ID: 0x00\n");
    snprintf(line, sizeof(line), "Firmware: %s\n", HACKEEG_FIRMWARE_VERSION);
    out.print(line);
    snprintf(line, sizeof(line), "Channels: %d\n$$$\n", max_channels);
    out.print(line);
}

static void runPipe(const char *body, Print *out, int source) {
    adsControlSetSource(source);
    char text[420];
    int code = adsControlExec(body, text, sizeof(text));
    char framed[460];
    snprintf(framed, sizeof(framed), "|%d %s|\r\n", code, text);
    emit(out, source, framed);
}

static void runChannel(const char *body, Print *out, int source) {
    if (strlen(body) < 7) {
        emit(out, source, "Failure: bad channel string$$$\n");
        return;
    }
    int channel = body[0] - '0';
    int powerDown = body[1] - '0';
    int gain = body[2] - '0';
    int input = body[3] - '0';
    int bias = body[4] - '0';
    int srb2 = body[5] - '0';
    int srb1 = body[6] - '0';
    char err[80];
    char line[96];
    if (!adsControlApplyChannel(channel, powerDown, gain, input, bias, srb2, srb1, err, sizeof(err))) {
        snprintf(line, sizeof(line), "Failure: %s$$$\n", err);
    } else {
        snprintf(line, sizeof(line), "Success: Channel set for %d$$$\n", channel);
    }
    emit(out, source, line);
}

static void runLeads(const char *body, Print *out, int source) {
    if (strlen(body) < 3) {
        emit(out, source, "Failure: bad lead-off string$$$\n");
        return;
    }
    int channel = body[0] - '0';
    int pOn = body[1] - '0';
    int nOn = body[2] - '0';
    char err[80];
    char line[96];
    if (!adsControlSetLeadOffChannel(channel, pOn, nOn, err, sizeof(err))) {
        snprintf(line, sizeof(line), "Failure: %s$$$\n", err);
    } else {
        snprintf(line, sizeof(line), "Success: Lead off set for %d$$$\n", channel);
    }
    emit(out, source, line);
}

static void runRate(char code, Print *out, int source) {
    char line[64];
    if (code == '~') {
        snprintf(line, sizeof(line), "Sample rate is %dHz$$$\n", adsControlSampleRateHz());
        emit(out, source, line);
        return;
    }
    const int rates[] = {16000, 8000, 4000, 2000, 1000, 500, 250};
    if (code < '0' || code > '6') {
        emit(out, source, "Failure: sample rate ~0..~6$$$\n");
        return;
    }
    char err[80];
    int hz = rates[code - '0'];
    if (!adsControlSetSampleRate(hz, err, sizeof(err))) {
        snprintf(line, sizeof(line), "Failure: %s$$$\n", err);
        emit(out, source, line);
        return;
    }
    snprintf(line, sizeof(line), "Sample rate set to %dHz$$$\n", hz);
    emit(out, source, line);
}

static void runPower(int channel, bool on, Print *out, int source) {
    char err[64];
    if (!adsControlSetPower(channel, on, err, sizeof(err))) {
        char line[96];
        snprintf(line, sizeof(line), "Failure: %s$$$\n", err);
        emit(out, source, line);
    }
}

static void runIdle(char c, Print *out, int source) {
    char line[160];
    switch (c) {
        case 'b':
            hackeegArmStream(streamSource(source), true);
            break;
        case 's':
            hackeegArmStream(streamSource(source), false);
            break;
        case 'v':
            hackeegRebootAds();
            if (!transportStreaming(source)) {
                ReplyPrint sink;
                writeBanner(sink);
                if (out != nullptr) {
                    writeBanner(*out);
                }
            }
            break;
        case 'V':
            snprintf(line, sizeof(line), "HackEEG %s$$$\n", HACKEEG_FIRMWARE_VERSION);
            emit(out, source, line);
            break;
        case '?':
            if (!transportStreaming(source)) {
                ReplyPrint sink;
                adsControlAppendRegisters(sink);
                if (out != nullptr) {
                    adsControlAppendRegisters(*out);
                }
            }
            break;
        case 'd':
            adsControlApplyDefaults();
            emit(out, source, "updating channel settings to default$$$\n");
            break;
        case 'D':
            // Six characters: power-down, gain, input, bias, SRB2, SRB1.
            // HackEEG referential default closes SRB1 and leaves SRB2 open.
            emit(out, source, "060101$$$\n");
            break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
            runPower(c - '0', false, out, source);
            break;
        case '!': runPower(1, true, out, source); break;
        case '@': runPower(2, true, out, source); break;
        case '#': runPower(3, true, out, source); break;
        case '$': runPower(4, true, out, source); break;
        case '%': runPower(5, true, out, source); break;
        case '^': runPower(6, true, out, source); break;
        case '&': runPower(7, true, out, source); break;
        case '*': runPower(8, true, out, source); break;
        case '0':
            adsControlSetTestMode("ground", line, sizeof(line));
            break;
        case '-':
            adsControlSetTestMode("slow", line, sizeof(line));
            break;
        case '=':
            adsControlSetTestMode("fast", line, sizeof(line));
            break;
        case '[':
            adsControlSetTestMode("slow2", line, sizeof(line));
            break;
        case ']':
            adsControlSetTestMode("fast2", line, sizeof(line));
            break;
        case 'p':
            adsControlSetTestMode("dc", line, sizeof(line));
            break;
        case 'c':
            emit(out, source, "Success: 8-channel packet$$$\n");
            break;
        case 'C':
            emit(out, source, "Failure: daisy not installed$$$\n");
            break;
        case 'A': case 'S': case 'F': case 'a': case 'j':
        case 'G': case 'H': case 'J': case 'K': case 'L':
            emit(out, source, "Failure: SD card not present$$$\n");
            break;
        case '`':
        case '<':
        case '>':
        case '{':
        case '}':
        case ':':
        case ';':
            break;
        default:
            break;
    }
}

static void feedByte(uint8_t c, Print *out, int source) {
    if (g_state != ST_IDLE && (millis() - g_state_ms) > 1000) {
        g_state = ST_IDLE;
        g_acc_len = 0;
    }
    if (g_state == ST_PIPE) {
        if (c == '|' || c == '\n') {
            g_acc[g_acc_len] = 0;
            g_state = ST_IDLE;
            runPipe(g_acc, out, source);
        } else if (g_acc_len + 1 < sizeof(g_acc) && c != '\r') {
            g_acc[g_acc_len++] = (char) c;
        }
        return;
    }
    if (g_state == ST_CHANNEL) {
        if (c == ' ' || c == '\t') {
            return;
        }
        if (c == 'X') {
            g_acc[g_acc_len] = 0;
            g_state = ST_IDLE;
            runChannel(g_acc, out, source);
        } else if (g_acc_len + 1 < sizeof(g_acc)) {
            g_acc[g_acc_len++] = (char) c;
        }
        return;
    }
    if (g_state == ST_LEADS) {
        if (c == ' ' || c == '\t') {
            return;
        }
        if (c == 'Z') {
            g_acc[g_acc_len] = 0;
            g_state = ST_IDLE;
            runLeads(g_acc, out, source);
        } else if (g_acc_len + 1 < sizeof(g_acc)) {
            g_acc[g_acc_len++] = (char) c;
        }
        return;
    }
    if (g_state == ST_RATE) {
        if (c == ' ' || c == '\t') {
            return;
        }
        g_state = ST_IDLE;
        runRate((char) c, out, source);
        return;
    }
    if (g_state == ST_BOARD) {
        g_state = ST_IDLE;
        emit(out, source, "Failure: board mode not implemented$$$\n");
        return;
    }
    if (c == '|') {
        enterState(ST_PIPE);
        return;
    }
    if (c == 'x') {
        enterState(ST_CHANNEL);
        return;
    }
    if (c == 'z') {
        enterState(ST_LEADS);
        return;
    }
    if (c == '~') {
        enterState(ST_RATE);
        return;
    }
    if (c == '/') {
        enterState(ST_BOARD);
        return;
    }
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
        return;
    }
    runIdle((char) c, out, source);
}

static void feedBytes(const uint8_t *data, size_t count, Print *out, int source) {
    for (size_t i = 0; i < count; i++) {
        feedByte(data[i], out, source);
    }
}

static bool jsonSpan(const String &body, const char *key, int &begin, int &end) {
    String pattern = String("\"") + key + "\"";
    int found = body.indexOf(pattern);
    if (found < 0) {
        return false;
    }
    int colon = body.indexOf(':', found + pattern.length());
    if (colon < 0) {
        return false;
    }
    int i = colon + 1;
    while (i < (int) body.length() && isspace((unsigned char) body[i])) {
        i++;
    }
    if (i >= (int) body.length()) {
        return false;
    }
    if (body[i] == '"') {
        int close = body.indexOf('"', i + 1);
        if (close < 0) {
            return false;
        }
        begin = i + 1;
        end = close;
        return true;
    }
    int j = i;
    while (j < (int) body.length() && body[j] != ',' && body[j] != '}' && !isspace((unsigned char) body[j])) {
        j++;
    }
    begin = i;
    end = j;
    return begin < end;
}

static bool jsonString(const String &body, const char *key, String &out) {
    int begin = 0;
    int end = 0;
    if (!jsonSpan(body, key, begin, end)) {
        return false;
    }
    out = body.substring(begin, end);
    return true;
}

static bool jsonInt(const String &body, const char *key, int &out) {
    String text;
    if (!jsonString(body, key, text)) {
        return false;
    }
    out = text.toInt();
    return true;
}

static bool jsonBool(const String &body, const char *key, bool &out) {
    String text;
    if (!jsonString(body, key, text)) {
        return false;
    }
    out = (text == "true" || text == "1");
    return true;
}

static void sendJson(int code, const char *body) {
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.send(code, "application/json", body);
}

static void sendOptions() {
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    g_server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    g_server.send(204);
}

static void formatIp(const IPAddress &ip, char *buf, size_t len) {
    snprintf(buf, len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void boardJson(char *buf, size_t len) {
    bool connected = adsControlBoardConnected();
    size_t used = (size_t) snprintf(buf, len,
                                    "{\"board_connected\":%s,\"board_type\":\"%s\",\"num_channels\":8,\"chip_channels\":%d,\"gains\":[",
                                    connected ? "true" : "false",
                                    connected ? "cyton" : "none",
                                    max_channels);
    for (int ch = 1; ch <= 8 && used + 8 < len; ch++) {
        int gain = (ch <= max_channels) ? adsControlGainMultiplier(ch) : 24;
        int wrote = snprintf(buf + used, len - used, "%s%d", ch == 1 ? "" : ",", gain);
        if (wrote < 0) {
            return;
        }
        used += (size_t) wrote;
    }
    snprintf(buf + used, len - used,
             "],\"ring_dropped\":%lu,\"drdy_missed\":%lu,\"radio_dropped\":%lu}",
             (unsigned long) sampleEngineRingDropped(),
             (unsigned long) sampleEngineDrdyMissed(),
             (unsigned long) sampleEngineRadioDropped());
}

static void tcpJson(char *buf, size_t len, bool connected) {
    char ip[20];
    formatIp(g_host, ip, sizeof(ip));
    snprintf(buf, len,
             "{\"connected\":%s,\"delimiter\":%s,\"ip\":\"%s\",\"output\":\"%s\",\"port\":%u,\"latency\":%d}",
             connected ? "true" : "false",
             g_delimiter ? "true" : "false",
             ip,
             g_json ? "json" : "raw",
             (unsigned) g_port,
             g_latency_us);
}

static void applyStreamBody(const String &body) {
    String ipText;
    if (jsonString(body, "ip", ipText)) {
        g_host.fromString(ipText);
    }
    int port = 0;
    if (jsonInt(body, "port", port) && port > 0 && port < 65536) {
        g_port = (uint16_t) port;
    }
    String output;
    if (jsonString(body, "output", output)) {
        g_json = (output == "json");
    }
    bool delimiter = g_delimiter;
    if (jsonBool(body, "delimiter", delimiter)) {
        g_delimiter = delimiter;
    }
    int latency = g_latency_us;
    if (jsonInt(body, "latency", latency) && latency > 0) {
        g_latency_us = latency;
    }
    bool repeat = false;
    if (jsonBool(body, "redundancy", repeat)) {
        g_udp_repeat = repeat;
    }
}

static void handleBoard() {
    char body[512];
    boardJson(body, sizeof(body));
    sendJson(200, body);
}

static void handleAll() {
    char board[512];
    boardJson(board, sizeof(board));
    char ip[20];
    formatIp(g_sta ? WiFi.localIP() : WiFi.softAPIP(), ip, sizeof(ip));
    char body[768];
    size_t n = strlen(board);
    if (n > 0 && board[n - 1] == '}') {
        board[n - 1] = 0;
    }
    snprintf(body, sizeof(body),
             "%s,\"ip\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"latency\":%d}",
             board, ip, HACKEEG_BT_NAME, HACKEEG_FIRMWARE_VERSION, g_latency_us);
    sendJson(200, body);
}

static void handleTcpPost() {
    String body = g_server.arg("plain");
    applyStreamBody(body);
    char json[256];
    if (g_port == 0 || g_host == IPAddress(0, 0, 0, 0)) {
        tcpJson(json, sizeof(json), false);
        sendJson(400, json);
        return;
    }
    g_tcp.stop();
    g_udp_on = false;
    g_tcp_on = g_tcp.connect(g_host, g_port);
    if (g_tcp_on) {
        g_tcp.setNoDelay(true);
    }
    tcpJson(json, sizeof(json), g_tcp_on);
    sendJson(g_tcp_on ? 200 : 504, json);
}

static void handleUdpPost() {
    String body = g_server.arg("plain");
    applyStreamBody(body);
    char json[256];
    if (g_port == 0 || g_host == IPAddress(0, 0, 0, 0)) {
        tcpJson(json, sizeof(json), false);
        sendJson(400, json);
        return;
    }
    g_tcp.stop();
    g_tcp_on = false;
    g_udp.begin(0);
    g_udp_on = true;
    tcpJson(json, sizeof(json), true);
    sendJson(200, json);
}

static void handleCommand() {
    String body = g_server.arg("plain");
    String command;
    if (!jsonString(body, "command", command) || command.length() == 0) {
        sendJson(400, "{\"ok\":false,\"error\":\"missing command\"}");
        return;
    }
    replyClear();
    feedBytes((const uint8_t *) command.c_str(), command.length(), nullptr, HACKEEG_STREAM_HTTP);
    char json[1200];
    size_t used = (size_t) snprintf(json, sizeof(json), "{\"ok\":true,\"response\":\"");
    for (size_t i = 0; i < g_reply_len && used + 2 < sizeof(json); i++) {
        char c = g_reply[i];
        if (c == '"' || c == '\\') {
            json[used++] = '\\';
        }
        if (c == '\n' || c == '\r') {
            continue;
        }
        json[used++] = c;
    }
    snprintf(json + used, sizeof(json) - used, "\"}");
    sendJson(200, json);
}

static void handleStreamStart() {
    hackeegArmStream(HACKEEG_STREAM_WIFI, true);
    sendJson(200, "{\"ok\":true}");
}

static void handleStreamStop() {
    hackeegArmStream(HACKEEG_STREAM_WIFI, false);
    sendJson(200, "{\"ok\":true}");
}

static void handleTcpDelete() {
    g_tcp.stop();
    g_tcp_on = false;
    char json[256];
    tcpJson(json, sizeof(json), false);
    sendJson(200, json);
}

static void handleUdpDelete() {
    g_udp_on = false;
    char json[256];
    tcpJson(json, sizeof(json), false);
    sendJson(200, json);
}

static void handleStats() {
    char body[192];
    snprintf(body, sizeof(body),
             "{\"ring_dropped\":%lu,\"drdy_missed\":%lu,\"radio_dropped\":%lu,\"produced\":%lu,\"sample_rate\":%d,\"streaming\":%s}",
             (unsigned long) sampleEngineRingDropped(),
             (unsigned long) sampleEngineDrdyMissed(),
             (unsigned long) sampleEngineRadioDropped(),
             (unsigned long) sampleEngineProduced(),
             adsControlSampleRateHz(),
             (hackeegWifiStreaming() || hackeegSppStreaming() || is_rdatac) ? "true" : "false");
    sendJson(200, body);
}

static int32_t sign24(const uint8_t *p) {
    int32_t value = ((int32_t) p[0] << 16) | ((int32_t) p[1] << 8) | p[2];
    if (value & 0x00800000) {
        value |= ~0x00FFFFFF;
    }
    return value;
}

static int64_t countsToNanovolts(int32_t code, int gain) {
    if (gain <= 0) {
        gain = 24;
    }
    return ((int64_t) code * 4500000000LL) / (8388607LL * (int64_t) gain);
}

static void writeJsonSample(Print &out, const HackeegSample &sample) {
    out.print("{\"chunk\":[{\"data\":[");
    for (int ch = 0; ch < 8; ch++) {
        if (ch) {
            out.print(',');
        }
        int gain = adsControlGainMultiplier(ch + 1);
        int64_t nv = countsToNanovolts(sign24(sample.packet + 2 + ch * 3), gain);
        out.print((long long) nv);
    }
    out.print("],\"timestamp\":");
    out.print(sample.timestamp);
    out.print("}]}");
    if (g_delimiter) {
        out.print("\r\n");
    }
}

static bool sendUdpPacket(const HackeegSample &sample) {
    int copies = g_udp_repeat ? 3 : 1;
    bool ok = false;
    for (int copy = 0; copy < copies; copy++) {
        if (!g_udp.beginPacket(g_host, g_port)) {
            continue;
        }
        if (g_json) {
            char buf[384];
            class BufPrint : public Print {
            public:
                char *dest;
                size_t cap;
                size_t len;
                explicit BufPrint(char *d, size_t c) : dest(d), cap(c), len(0) { dest[0] = 0; }
                size_t write(uint8_t c) override {
                    if (len + 1 < cap) {
                        dest[len++] = (char) c;
                        dest[len] = 0;
                    }
                    return 1;
                }
            } printer(buf, sizeof(buf));
            writeJsonSample(printer, sample);
            g_udp.write((const uint8_t *) buf, printer.len);
        } else {
            g_udp.write(sample.packet, 33);
        }
        if (g_udp.endPacket()) {
            ok = true;
        }
    }
    return ok;
}

static void pollSsdp() {
    int packetSize = g_ssdp.parsePacket();
    if (packetSize <= 0) {
        return;
    }
    char buf[256];
    int len = g_ssdp.read(buf, sizeof(buf) - 1);
    if (len <= 0) {
        return;
    }
    buf[len] = 0;
    if (strstr(buf, "M-SEARCH") == nullptr) {
        return;
    }
    char ip[20];
    formatIp(g_sta ? WiFi.localIP() : WiFi.softAPIP(), ip, sizeof(ip));
    char response[320];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "CACHE-CONTROL: max-age=1800\r\n"
             "LOCATION: http://%s\r\n"
             "ST: urn:schemas-upnp-org:device:Basic:1\r\n"
             "USN: uuid:hackeeg::urn:schemas-upnp-org:device:Basic:1\r\n"
             "\r\n",
             ip);
    g_ssdp.beginPacket(g_ssdp.remoteIP(), g_ssdp.remotePort());
    g_ssdp.write((const uint8_t *) response, strlen(response));
    g_ssdp.endPacket();
}

static void serviceSpp() {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    bool up = g_bt.hasClient();
    if (up && !g_spp_was_up && !hackeegSppStreaming()) {
        writeBanner(g_bt);
    }
    if (!up && g_spp_was_up && hackeegSppStreaming()) {
        hackeegArmStream(HACKEEG_STREAM_SPP, false);
    }
    g_spp_was_up = up;
    if (!up) {
        return;
    }
    while (g_bt.available() > 0) {
        int value = g_bt.read();
        if (value < 0) {
            break;
        }
        feedByte((uint8_t) value, &g_bt, HACKEEG_STREAM_SPP);
    }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

static void serviceTcp() {
    if (g_tcp_on && !g_tcp.connected()) {
        g_tcp_on = false;
        g_tcp.stop();
    }
    if (!g_tcp.connected()) {
        return;
    }
    while (g_tcp.available() > 0) {
        int value = g_tcp.read();
        if (value < 0) {
            break;
        }
        feedByte((uint8_t) value, &g_tcp, HACKEEG_STREAM_TCP);
    }
}

static void pumpSamples() {
    HackeegSample sample;
    for (int n = 0; n < 96; n++) {
        if (!sampleEnginePop(&sample)) {
            return;
        }
        hackeegEmitUsbSample(sample.timestamp, sample.sampleNumber, sample.raw, sample.rawLen);
        if (hackeegWifiStreaming()) {
            bool sent = false;
            if (g_tcp_on && g_tcp.connected()) {
                if (g_json) {
                    writeJsonSample(g_tcp, sample);
                    sent = true;
                } else {
                    size_t wrote = g_tcp.write(sample.packet, 33);
                    sent = wrote == 33;
                }
            }
            if (g_udp_on) {
                sent = sendUdpPacket(sample) || sent;
            }
            if (!sent) {
                sampleEngineAddRadioDropped(1);
            }
        }
        if (hackeegSppStreaming()) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            if (g_bt.hasClient()) {
                size_t wrote = g_bt.write(sample.packet, 33);
                if (wrote < 33) {
                    sampleEngineAddRadioDropped(1);
                }
            } else {
                sampleEngineAddRadioDropped(1);
            }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        }
    }
}

static void startRadio() {
    WiFi.persistent(false);
    WiFi.setSleep(false);
    g_sta = false;
    if (HACKEEG_STA_SSID[0] != '\0') {
        WiFi.mode(WIFI_STA);
        WiFi.begin(HACKEEG_STA_SSID, HACKEEG_STA_PASSWORD);
        HACKEEG_SERIAL.print("# joining ");
        HACKEEG_SERIAL.println(HACKEEG_STA_SSID);
        uint32_t started = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - started < 12000) {
            delay(250);
            HACKEEG_SERIAL.print('.');
        }
        HACKEEG_SERIAL.println();
        g_sta = WiFi.status() == WL_CONNECTED;
    }
    if (!g_sta) {
        WiFi.mode(WIFI_AP);
        IPAddress ap(192, 168, 4, 1);
        WiFi.softAPConfig(ap, ap, IPAddress(255, 255, 255, 0));
        WiFi.softAP(HACKEEG_AP_SSID, HACKEEG_AP_PASSWORD, 6);
        HACKEEG_SERIAL.print("# AP ");
        HACKEEG_SERIAL.print(HACKEEG_AP_SSID);
        HACKEEG_SERIAL.print(" password ");
        HACKEEG_SERIAL.println(HACKEEG_AP_PASSWORD);
    }
    IPAddress ip = g_sta ? WiFi.localIP() : WiFi.softAPIP();
    char ipText[20];
    formatIp(ip, ipText, sizeof(ipText));
    HACKEEG_SERIAL.print("# HTTP http://");
    HACKEEG_SERIAL.print(ipText);
    HACKEEG_SERIAL.println("/board");

    g_ssdp.beginMulticast(IPAddress(239, 255, 255, 250), 1900);

    g_server.on("/board", HTTP_GET, handleBoard);
    g_server.on("/board", HTTP_OPTIONS, sendOptions);
    g_server.on("/all", HTTP_GET, handleAll);
    g_server.on("/all", HTTP_OPTIONS, sendOptions);
    g_server.on("/stats", HTTP_GET, handleStats);
    g_server.on("/stats", HTTP_OPTIONS, sendOptions);
    g_server.on("/version", HTTP_GET, []() {
        g_server.sendHeader("Access-Control-Allow-Origin", "*");
        g_server.send(200, "text/plain", HACKEEG_FIRMWARE_VERSION);
    });
    g_server.on("/tcp", HTTP_GET, []() {
        char json[256];
        tcpJson(json, sizeof(json), g_tcp.connected());
        sendJson(200, json);
    });
    g_server.on("/tcp", HTTP_POST, handleTcpPost);
    g_server.on("/tcp", HTTP_DELETE, handleTcpDelete);
    g_server.on("/tcp", HTTP_OPTIONS, sendOptions);
    g_server.on("/udp", HTTP_POST, handleUdpPost);
    g_server.on("/udp", HTTP_DELETE, handleUdpDelete);
    g_server.on("/udp", HTTP_OPTIONS, sendOptions);
    g_server.on("/command", HTTP_POST, handleCommand);
    g_server.on("/command", HTTP_OPTIONS, sendOptions);
    g_server.on("/stream/start", HTTP_GET, handleStreamStart);
    g_server.on("/stream/start", HTTP_OPTIONS, sendOptions);
    g_server.on("/stream/stop", HTTP_GET, handleStreamStop);
    g_server.on("/stream/stop", HTTP_OPTIONS, sendOptions);
    g_server.on("/", HTTP_GET, []() {
        g_server.sendHeader("Access-Control-Allow-Origin", "*");
        g_server.send(200, "text/plain", "HackEEG OpenBCI. GET /board");
    });
    g_server.begin();

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (!g_bt.begin(HACKEEG_BT_NAME)) {
        HACKEEG_SERIAL.println("# Classic Bluetooth SPP failed to start");
    } else {
        HACKEEG_SERIAL.print("# SPP name ");
        HACKEEG_SERIAL.println(HACKEEG_BT_NAME);
    }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

void openBciSetup() {
    HACKEEG_SERIAL.print("# pins SCK=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_SCK);
    HACKEEG_SERIAL.print(" MOSI=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_MOSI);
    HACKEEG_SERIAL.print(" MISO=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_MISO);
    HACKEEG_SERIAL.print(" CS=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_CS);
    HACKEEG_SERIAL.print(" DRDY=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_DRDY);
    HACKEEG_SERIAL.print(" START=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_START);
    HACKEEG_SERIAL.print(" RESET=");
    HACKEEG_SERIAL.print(HACKEEG_PIN_RESET);
    HACKEEG_SERIAL.print(" PWDN=");
    HACKEEG_SERIAL.println(HACKEEG_PIN_PWDN);
    startRadio();
}

void openBciLoop() {
    g_server.handleClient();
    pollSsdp();
    serviceSpp();
    serviceTcp();
    pumpSamples();
}

#endif
