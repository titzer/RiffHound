#pragma once
#include <stdint.h>

// Called for each complete MIDI message (channel voice or realtime).
// Realtime messages arrive as len=1 with msg[0] >= 0xF8.
// ts is a platform timestamp (nanoseconds on macOS, 0 on some platforms).
typedef void (*midi_msg_handler_t)(uint64_t ts,
                                   const uint8_t *msg, int len,
                                   void *userdata);

// Connect to all available MIDI sources and deliver messages to handler.
// Blocks until the process is killed (Ctrl-C).
int midi_run(midi_msg_handler_t handler, void *userdata);
