#include "sd.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// Cardputer-Adv SPI pins for the microSD slot (from official pinmap).
#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN   12

namespace {

bool g_available = false;
bool g_paused    = false;

}  // namespace

namespace sd {

void begin() {
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        g_available = true;
        Serial.printf("[sd] card mounted, size=%lluMB\n",
                      SD.cardSize() / (1024 * 1024));
    } else {
        Serial.println("[sd] no card or mount failed");
    }
}

bool isAvailable() { return g_available && !g_paused; }

void pause() {
    if (g_paused) return;
    SD.end();
    SPI.end();
    g_paused = true;
    Serial.println("[sd] paused");
}

void resume() {
    if (!g_paused) return;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    g_available = SD.begin(SD_SPI_CS_PIN, SPI, 25000000);
    g_paused    = false;
    Serial.printf("[sd] resumed, available=%d\n", (int)g_available);
}

bool isPaused() { return g_paused; }

}  // namespace sd
