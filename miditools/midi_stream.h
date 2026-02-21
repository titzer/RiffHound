#pragma once
#include <stdint.h>
#include "midi_platform.h"

// Running-status parse state. Allocate one per MIDI input stream.
typedef struct {
    uint8_t running_status;
    uint8_t msg_buf[3];
    int     msg_need;   // expected length of current message
    int     msg_have;   // bytes accumulated so far
} midi_parse_state_t;

void midi_parse_state_init(midi_parse_state_t *s);

// Feed a raw byte buffer from one platform packet/chunk through the parser.
// handler is called for each complete message (channel voice or realtime).
void midi_parse_bytes(midi_parse_state_t *s,
                      const uint8_t *data, int len, uint64_t ts,
                      midi_msg_handler_t handler, void *userdata);
