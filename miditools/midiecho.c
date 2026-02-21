#include "midi_platform.h"
#include "midi_decode.h"

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#define NOTE_COL 0
#define CC_COL   60   // column offset where CC messages are printed

static void print_msg_line(int show_ts, uint64_t ts, const uint8_t *msg, int n) {
    if (show_ts) printf("\r%12" PRIu64 " : ", ts);
    else         printf("\r                ");  // 16 chars to match timestamp width

    int base_col   = 15;
    int cur_col    = base_col;
    uint8_t st     = msg[0];
    uint8_t hi     = st & 0xF0;
    uint8_t ch     = (st & 0x0F) + 1;
    int is_cc      = (hi == 0xB0 && n >= 3);
    int is_note    = ((hi == 0x90 || hi == 0x80) && n >= 3);
    int target_col = base_col + (is_cc ? CC_COL : NOTE_COL);

    while (cur_col < target_col) { putchar(' '); cur_col++; }

    printf("ch=%d ", ch);
    for (int i = 0; i < 3; i++) {
        if (i < n) printf("%02X", msg[i]);
        else       printf("  ");
        if (i != 2) putchar(' ');
    }

    if (is_note) {
        uint8_t note = msg[1];
        uint8_t vel  = msg[2];
        char full[8];
        snprintf(full, sizeof(full), "%s%d", note_name(note), note_octave(note));
        if (hi == 0x90 && vel != 0)
            printf("   NOTE_ON   %-4s", full);
        else
            printf("   NOTE_OFF  %-4s", full);
    } else if (is_cc) {
        printf("   CC ch=%u ctrl=%u val=%u", ch, msg[1], msg[2]);
    }

    putchar('\n');
    fflush(stdout);
}

static void show_realtime(uint64_t ts, uint8_t b) {
    // Overwrite current line; realtime bytes don't advance to a new line.
    printf("\r%12" PRIu64 " : %02X %s", ts, b, realtime_name(b));
    fflush(stdout);
}

static void on_midi(uint64_t ts, const uint8_t *msg, int len, void *userdata) {
    (void)userdata;

    if (is_realtime(msg[0])) {
        show_realtime(ts, msg[0]);
        return;
    }

    // Show timestamp only when it changes (i.e. first message of each platform packet).
    static uint64_t last_ts = 0;
    int show_ts = (ts != last_ts);
    last_ts = ts;

    print_msg_line(show_ts, ts, msg, len);
}

int main(void) {
    return midi_run(on_midi, NULL);
}
