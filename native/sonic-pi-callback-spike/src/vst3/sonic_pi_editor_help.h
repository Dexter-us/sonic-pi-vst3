// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <string_view>

namespace SonicPiVst3 {

struct CodeHelpTopic {
    std::string_view key;
    std::string_view menuLabel;
    std::string_view summary;
    std::string_view example;
};

inline constexpr std::array<CodeHelpTopic, 19> kCodeHelpTopics{{
    {"play", "play - notes and chords",
     "Play a note or chord. Common options: amp, pan, attack, sustain, "
     "release, cutoff, and resonance.",
     "play 60, amp: 0.5, release: 0.2"},
    {"play_chord", "play_chord - notes together",
     "Play several pitches at once. Use play_pattern_timed when notes should "
     "sound one after another.",
     "play_chord [60, 64, 67], sustain: 0.5"},
    {"sample", "sample - audio samples",
     "Trigger a bundled sample. Common options: amp, rate, start, finish, "
     "pan, and beat_stretch.",
     "sample :bd_haus, amp: 0.8"},
    {"live_loop", "live_loop - repeating code",
     "Run a named loop repeatedly. Use sleep for a fixed beat gap or sync to "
     "another loop's cue.",
     "live_loop :beat do\n  sample :bd_haus\n  sleep 1\nend"},
    {"with_fx", "with_fx - audio effects",
     "Apply an effect to everything inside the block. Use mix to blend the "
     "processed and dry signal.",
     "with_fx :reverb, mix: 0.35 do\n  play 60\nend"},
    {"use_synth", "use_synth - choose a synth",
     "Choose the sound used by later play calls in this thread. Synths expose "
     "different options.",
     "use_synth :prophet\nplay 60"},
    {"use_bpm", "use_bpm - set tempo",
     "Set the current thread's tempo in beats per minute. Sleep durations are "
     "measured in beats.",
     "use_bpm 110"},
    {"sleep", "sleep - wait in beats",
     "Pause this thread and yield to Sonic Pi's scheduler. A duration of 1 "
     "means one beat at the current BPM.",
     "play 60\nsleep 0.5"},
    {"sync", "sync - wait for a cue",
     "Pause this thread until another thread or loop sends the matching cue.",
     "sync :drum_hit"},
    {"cue", "cue - signal other loops",
     "Send a named timing signal. A loop waiting with sync can start on that "
     "signal.",
     "cue :drum_hit"},
    {"prophet", ":prophet - synth",
     "A broad, layered synth suited to pads and melodic parts. Try amp, "
     "release, cutoff, and resonance.",
     "use_synth :prophet\nplay 60, release: 1"},
    {"tb303", ":tb303 - synth",
     "A resonant bass synth. Try cutoff, resonance, wave, and envelope "
     "controls.",
     "use_synth :tb303\nplay 36, cutoff: 75"},
    {"fm", ":fm - synth",
     "An FM synth with bright, metallic tones. Try divisor and depth along "
     "with normal note envelope options.",
     "use_synth :fm\nplay 72, divisor: 2"},
    {"reverb", ":reverb - effect",
     "Add room ambience. Use mix to set the wet/dry balance and room to "
     "adjust the space.",
     "with_fx :reverb, mix: 0.4 do\n  play 60\nend"},
    {"echo", ":echo - effect",
     "Repeat the incoming sound. Phase controls the gap between repeats; "
     "decay controls how long they continue.",
     "with_fx :echo, phase: 0.25, decay: 2 do\n  play 60\nend"},
    {"slicer", ":slicer - effect",
     "Rhythmically gate or pan the sound. Try phase, pulse_width, and mix.",
     "with_fx :slicer, phase: 0.25 do\n  play 60, sustain: 2\nend"},
    {"bd_haus", ":bd_haus - sample",
     "A short electronic kick sample. Adjust amp, rate, and pan for variation.",
     "sample :bd_haus, amp: 0.8"},
    {"drum_snare_hard", ":drum_snare_hard - sample",
     "A sharp snare hit from Sonic Pi's bundled drum samples.",
     "sample :drum_snare_hard"},
    {"loop_amen", ":loop_amen - sample",
     "A classic breakbeat loop. Use beat_stretch to fit it to a chosen beat "
     "length or rate to change its speed and pitch.",
     "sample :loop_amen, beat_stretch: 4"},
}};

inline const CodeHelpTopic* findCodeHelpTopic(std::string_view selection) {
    while (!selection.empty() &&
           (selection.front() == ' ' || selection.front() == '\t' ||
            selection.front() == '\r' || selection.front() == '\n')) {
        selection.remove_prefix(1);
    }
    while (!selection.empty() &&
           (selection.back() == ' ' || selection.back() == '\t' ||
            selection.back() == '\r' || selection.back() == '\n')) {
        selection.remove_suffix(1);
    }
    if (!selection.empty() && selection.front() == ':') {
        selection.remove_prefix(1);
    }

    for (const auto& topic : kCodeHelpTopics) {
        if (selection == topic.key) return &topic;
    }
    return nullptr;
}

} // namespace SonicPiVst3
