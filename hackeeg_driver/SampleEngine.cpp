#include "SampleEngine.h"

#if defined(ESP32)

#include <string.h>

#include "AdsControl.h"
#include "DriverState.h"
#include "SpiDma.h"
#include "adsCommand.h"

static TaskHandle_t g_task = nullptr;
static volatile uint8_t g_capture = 0;
static volatile uint32_t g_sample_number = 0;
static volatile uint32_t g_ring_dropped = 0;
static volatile uint32_t g_drdy_missed = 0;
static volatile uint32_t g_radio_dropped = 0;
static volatile uint32_t g_produced = 0;

static const uint16_t kRingSize = 512;
static HackeegSample g_ring[kRingSize];
static volatile uint16_t g_head = 0;
static volatile uint16_t g_tail = 0;
static portMUX_TYPE g_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static void pushSample(const HackeegSample &sample) {
    portENTER_CRITICAL(&g_ring_mux);
    uint16_t next = (uint16_t) ((g_head + 1) % kRingSize);
    if (next == g_tail) {
        g_ring_dropped++;
        portEXIT_CRITICAL(&g_ring_mux);
        return;
    }
    g_ring[g_head] = sample;
    g_head = next;
    g_produced++;
    portEXIT_CRITICAL(&g_ring_mux);
}

static void buildPacket(HackeegSample *sample, int nbytes, uint8_t power) {
    memset(sample->packet, 0, sizeof(sample->packet));
    sample->packet[0] = 0xA0;
    sample->packet[1] = (uint8_t) sample->sampleNumber;
    int channels = max_channels;
    if (channels > 8) {
        channels = 8;
    }
    for (int ch = 0; ch < 8; ch++) {
        int dst = 2 + ch * 3;
        int src = 3 + ch * 3;
        if (ch < channels && (power & (1u << ch)) && src + 2 < nbytes) {
            sample->packet[dst] = sample->raw[src];
            sample->packet[dst + 1] = sample->raw[src + 1];
            sample->packet[dst + 2] = sample->raw[src + 2];
        }
    }
    sample->packet[32] = 0xC0;
}

static void sampleTask(void *) {
    for (;;) {
        uint32_t notes = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!g_capture || notes == 0) {
            continue;
        }
        if (notes > 1) {
            g_drdy_missed += (notes - 1);
        }
        adsBusLock();
        if (!g_capture) {
            adsBusUnlock();
            continue;
        }
        int nbytes = num_spi_bytes;
        if (nbytes < 3) {
            nbytes = 3;
        }
        if (nbytes > 27) {
            nbytes = 27;
        }
        HackeegSample sample;
        memset(&sample, 0, sizeof(sample));
        digitalWrite(PIN_CS, LOW);
        delayMicroseconds(4);
        spiRec(sample.raw, (size_t) nbytes);
        digitalWrite(PIN_CS, HIGH);
        uint32_t sn = g_sample_number + (notes - 1);
        g_sample_number += notes;
        sample.timestamp = micros();
        sample.sampleNumber = sn;
        sample.rawLen = (uint8_t) nbytes;
        buildPacket(&sample, nbytes, adsControlPowerMask());
        if (g_capture) {
            pushSample(sample);
        }
        adsBusUnlock();
    }
}

void ARDUINO_ISR_ATTR sampleEngineIsr() {
    if (g_task == nullptr) {
        return;
    }
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(g_task, &wake);
    if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void sampleEngineBegin() {
    if (g_task != nullptr) {
        return;
    }
    xTaskCreatePinnedToCore(sampleTask, "ads_drdy", 8192, nullptr, 4, &g_task, 1);
}

void sampleEngineAttach() {
    if (g_task == nullptr) {
        sampleEngineBegin();
    }
    pinMode(IPIN_DRDY, INPUT);
    detachInterrupt(digitalPinToInterrupt(IPIN_DRDY));
    attachInterrupt(digitalPinToInterrupt(IPIN_DRDY), sampleEngineIsr, FALLING);
}

void sampleEngineSetCapture(bool on) {
    g_capture = on ? 1 : 0;
}

void sampleEngineResetCounter() {
    g_sample_number = 0;
}

void sampleEngineClearNotify() {
    if (g_task != nullptr) {
        ulTaskNotifyValueClear(g_task, 0xFFFFFFFF);
    }
}

void sampleEngineDiscard() {
    portENTER_CRITICAL(&g_ring_mux);
    g_tail = g_head;
    portEXIT_CRITICAL(&g_ring_mux);
}

bool sampleEnginePop(HackeegSample *out) {
    if (!out) {
        return false;
    }
    portENTER_CRITICAL(&g_ring_mux);
    if (g_tail == g_head) {
        portEXIT_CRITICAL(&g_ring_mux);
        return false;
    }
    *out = g_ring[g_tail];
    g_tail = (uint16_t) ((g_tail + 1) % kRingSize);
    portEXIT_CRITICAL(&g_ring_mux);
    return true;
}

uint32_t sampleEngineRingDropped() { return g_ring_dropped; }
uint32_t sampleEngineDrdyMissed() { return g_drdy_missed; }
uint32_t sampleEngineRadioDropped() { return g_radio_dropped; }
uint32_t sampleEngineProduced() { return g_produced; }

void sampleEngineAddRadioDropped(uint32_t count) {
    g_radio_dropped += count;
}

#endif
