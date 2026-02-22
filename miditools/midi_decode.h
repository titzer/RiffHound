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

// ── Channel voice message field accessors ─────────────────────────────────────

// Channel number 1-16.
static inline int msg_channel(const uint8_t *msg) {
    return (msg[0] & 0x0F) + 1;
}

// High nibble of the status byte (0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0).
static inline uint8_t msg_type(const uint8_t *msg) {
    return msg[0] & 0xF0;
}

// True if msg is a Note On or Note Off (including 0x90 with vel=0).
static inline int msg_is_note(const uint8_t *msg, int len) {
    uint8_t hi = msg[0] & 0xF0;
    return (hi == 0x90 || hi == 0x80) && len >= 3;
}

// True if msg is a sounding note-on (0x90 with velocity > 0).
static inline int msg_is_note_on(const uint8_t *msg, int len) {
    return (msg[0] & 0xF0) == 0x90 && len >= 3 && msg[2] != 0;
}

// True if msg is a note-off (0x80, or 0x90 with velocity == 0).
static inline int msg_is_note_off(const uint8_t *msg, int len) {
    return ((msg[0] & 0xF0) == 0x80 && len >= 3) ||
           ((msg[0] & 0xF0) == 0x90 && len >= 3 && msg[2] == 0);
}

// True if msg is a Control Change.
static inline int msg_is_cc(const uint8_t *msg, int len) {
    return (msg[0] & 0xF0) == 0xB0 && len >= 3;
}

// Note number (valid when msg_is_note).
static inline uint8_t msg_note_num(const uint8_t *msg) { return msg[1]; }

// Velocity (valid when msg_is_note).
static inline uint8_t msg_velocity(const uint8_t *msg)  { return msg[2]; }

// CC controller number (valid when msg_is_cc).
static inline uint8_t msg_cc_num(const uint8_t *msg) { return msg[1]; }

// CC value (valid when msg_is_cc).
static inline uint8_t msg_cc_val(const uint8_t *msg) { return msg[2]; }

// ── Note number helpers ───────────────────────────────────────────────────────

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
