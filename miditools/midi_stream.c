#include "midi_stream.h"
#include "midi_decode.h"
#include <string.h>

void midi_parse_state_init(midi_parse_state_t *s) {
    memset(s, 0, sizeof(*s));
}

void midi_parse_bytes(midi_parse_state_t *s,
                      const uint8_t *data, int len, uint64_t ts,
                      midi_msg_handler_t handler, void *userdata) {
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];

        // Real-time bytes are single-byte, interleave anywhere, pass straight through.
        if (is_realtime(b)) {
            handler(ts, &b, 1, userdata);
            continue;
        }

        if (b & 0x80) {
            // Non-realtime status byte.
            int mlen = msg_len_from_status(b);
            if (mlen > 0) {
                s->running_status = b;
                s->msg_buf[0] = b;
                s->msg_have = 1;
                s->msg_need = mlen;
            } else {
                // SysEx or system common: drop and clear running status.
                s->running_status = 0;
                s->msg_have = 0;
                s->msg_need = 0;
            }
        } else {
            // Data byte.
            if (s->msg_need == 0) {
                // No message in progress; try running status.
                int mlen = msg_len_from_status(s->running_status);
                if (mlen > 0) {
                    s->msg_buf[0] = s->running_status;
                    s->msg_have = 1;
                    s->msg_need = mlen;
                } else {
                    continue; // nothing to do with this byte
                }
            }

            if (s->msg_have < 3) s->msg_buf[s->msg_have] = b;
            s->msg_have++;

            if (s->msg_have >= s->msg_need) {
                handler(ts, s->msg_buf, s->msg_need, userdata);
                // Keep running_status; reset accumulator for next message.
                s->msg_have = 0;
                s->msg_need = 0;
            }
        }
    }
}
