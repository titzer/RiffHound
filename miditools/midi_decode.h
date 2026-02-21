#pragma once
#include <stdint.h>

// Returns the expected byte length of a channel voice message given its status byte,
// or 0 if the status is not a recognized channel voice message.
static inline int msg_len_from_status(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: return 3; // Note Off
        case 0x90: return 3; // Note On
        case 0xA0: return 3; // Poly Pressure
        case 0xB0: return 3; // Control Change
        case 0xC0: return 2; // Program Change
        case 0xD0: return 2; // Channel Pressure
        case 0xE0: return 3; // Pitch Bend
        default:   return 0;
    }
}

// True if b is a System Real-Time status byte (0xF8..0xFF).
static inline int is_realtime(uint8_t b) {
    return b >= 0xF8;
}

static inline const char *realtime_name(uint8_t b) {
    switch (b) {
        case 0xF8: return "TIMING_CLOCK";
        case 0xFA: return "START";
        case 0xFB: return "CONTINUE";
        case 0xFC: return "STOP";
        case 0xFE: return "ACTIVE_SENSE";
        case 0xFF: return "RESET";
        case 0xF9: return "RT_UNDEF_F9";
        case 0xFD: return "RT_UNDEF_FD";
        default:   return "RT_OTHER";
    }
}

// Returns the pitch-class name for a MIDI note number (e.g. "C#").
static inline const char *note_name(uint8_t note) {
    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return names[note % 12];
}

// Returns the octave number for a MIDI note number (middle C = C4 = note 60).
static inline int note_octave(uint8_t note) {
    return (note / 12) - 1;
}
