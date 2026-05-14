#pragma once

namespace audio {

enum class Cue {
    Attention,   // two-tone soft chime
    Approve,     // bright pluck
    Deny,        // low muted thud
    Celebrate,   // ascending arpeggio
};

void begin();
void play(Cue c);  // no-op if settings::audioEnabled() is false

}  // namespace audio
