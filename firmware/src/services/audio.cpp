#include "audio.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "settings.h"

namespace audio {

namespace {

void blip(int freq_hz, int ms) {
    M5Cardputer.Speaker.tone(freq_hz, ms);
}

void chain(const int (&seq)[8], int count) {
    for (int i = 0; i < count; i++) {
        if (seq[i] <= 0) { delay(40); continue; }
        blip(seq[i], 70);
        delay(75);
    }
}

}  // namespace

void begin() {
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(80);  // 0..255
}

void play(Cue c) {
    if (!settings::audioEnabled()) return;

    switch (c) {
        case Cue::Attention: {
            const int seq[8] = {880, 1320, 0, 0, 0, 0, 0, 0};
            chain(seq, 3);
            break;
        }
        case Cue::Approve: {
            const int seq[8] = {880, 1175, 1480, 0, 0, 0, 0, 0};
            chain(seq, 3);
            break;
        }
        case Cue::Deny: {
            const int seq[8] = {220, 165, 0, 0, 0, 0, 0, 0};
            chain(seq, 2);
            break;
        }
        case Cue::Celebrate: {
            const int seq[8] = {523, 659, 784, 1047, 0, 0, 0, 0};
            chain(seq, 4);
            break;
        }
    }
}

}  // namespace audio
