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
    int target_col = base_col + (msg_is_cc(msg, n) ? CC_COL : NOTE_COL);

    while (cur_col < target_col) { putchar(' '); cur_col++; }

    printf("ch=%d ", msg_channel(msg));
    for (int i = 0; i < 3; i++) {
        if (i < n) printf("%02X", msg[i]);
        else       printf("  ");
        if (i != 2) putchar(' ');
    }

    if (msg_is_note(msg, n)) {
        char full[8];
        snprintf(full, sizeof(full), "%s%d",
                 note_name(msg_note_num(msg)), note_octave(msg_note_num(msg)));
        if (msg_is_note_on(msg, n))
            printf("   NOTE_ON   %-4s", full);
        else
            printf("   NOTE_OFF  %-4s", full);
    } else if (msg_is_cc(msg, n)) {
        printf("   CC ch=%u ctrl=%u val=%u",
               msg_channel(msg), msg_cc_num(msg), msg_cc_val(msg));
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
