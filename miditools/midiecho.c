#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

static void print_cfstring(CFStringRef s) {
    if (!s) { printf("(null)"); return; }
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) printf("%s", buf);
    else printf("(unprintable)");
}

static int msg_len_from_status(uint8_t status) {
    // Channel Voice messages
    uint8_t hi = status & 0xF0;
    switch (hi) {
        case 0x80: return 3; // Note Off
        case 0x90: return 3; // Note On
        case 0xA0: return 3; // Poly Pressure
        case 0xB0: return 3; // CC
        case 0xC0: return 2; // Program Change
        case 0xD0: return 2; // Channel Pressure
        case 0xE0: return 3; // Pitch Bend
        default:   return 0;
    }
}

static int is_realtime_status(uint8_t b) {
    // System Real-Time: 0xF8..0xFF excluding 0xF9 and 0xFD (undefined but still real-time class)
    return (b >= 0xF8);
}

static const char* realtime_name(uint8_t b) {
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

#define NOTE_COL 0
#define CC_COL   60   // column where CC messages start

static void print_msg_line(int show_ts, uint64_t ts, const uint8_t *msg, int n) {
    if (show_ts) printf("\r%12" PRIu64 " : ", ts);
    else         printf("\r             ");  // 15 chars

    int base_col = 15;  // timestamp width including " : "
    int cur_col  = base_col;

    uint8_t st = msg[0];
    uint8_t hi = st & 0xF0;
    uint8_t ch = (st & 0x0F) + 1;

    int is_cc = (hi == 0xB0 && n >= 3);
    int is_note = ((hi == 0x90 || hi == 0x80) && n >= 3);

    // Decide target column
    int target_col = base_col + (is_cc ? CC_COL : NOTE_COL);

    // Pad to target column
    while (cur_col < target_col) {
        putchar(' ');
        cur_col++;
    }

    // Print hex bytes
    for (int i = 0; i < 3; i++) {
        if (i < n) printf("%02X", msg[i]);
        else       printf("  ");
        if (i != 2) putchar(' ');
    }

    // Decode
    if (is_note) {
        uint8_t note = msg[1];
        uint8_t vel  = msg[2];

        if (hi == 0x90 && vel != 0)
            printf("   NOTE_ON   ch=%u note=%u", ch, note);
        else
            printf("   NOTE_OFF  ch=%u note=%u", ch, note);

    } else if (is_cc) {
        printf("   CC ch=%u ctrl=%u val=%u", ch, msg[1], msg[2]);
    }

    putchar('\n');
    fflush(stdout);
}


static void show_realtime_status(uint64_t ts, uint8_t b) {
    // "Status messages go at the beginning of the line and update the time only (no extra line)."
    // We overwrite the current line using carriage return.
    printf("\r%12" PRIu64 " : %02X %s", ts, b, realtime_name(b));
    fflush(stdout);
}

static void midi_read_proc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon) {
    (void)refCon;
    (void)connRefCon;

    const MIDIPacket *pkt = &pktlist->packet[0];

    // Running status state across packets (simple/typical devices will be fine).
    static uint8_t running_status = 0;
    static uint8_t msg_buf[3];
    static int     msg_need = 0;
    static int     msg_have = 0;

    for (UInt32 pi = 0; pi < pktlist->numPackets; pi++) {
        uint64_t ts = (uint64_t)pkt->timeStamp;
        const uint8_t *d = pkt->data;
        int len = (int)pkt->length;

        int printed_any_lines_for_this_packet = 0;

        for (int i = 0; i < len; i++) {
            uint8_t b = d[i];

            // Real-time/status bytes can appear anywhere and are single-byte.
            if (is_realtime_status(b)) {
                show_realtime_status(ts, b);
                continue;
            }

            if (b & 0x80) {
                // Status byte (non real-time)
                int mlen = msg_len_from_status(b);
                if (mlen > 0) {
                    running_status = b;
                    msg_buf[0] = b;
                    msg_have = 1;
                    msg_need = mlen;
                } else {
                    // For minimalism: ignore system common / sysex etc.
                    running_status = 0;
                    msg_have = 0;
                    msg_need = 0;
                }
            } else {
                // Data byte
                if (msg_need == 0) {
                    // Maybe running status applies
                    int mlen = msg_len_from_status(running_status);
                    if (mlen > 0) {
                        msg_buf[0] = running_status;
                        msg_have = 1;
                        msg_need = mlen;
                    } else {
                        continue; // can't interpret
                    }
                }

                if (msg_have < 3) msg_buf[msg_have] = b;
                msg_have++;

                if (msg_need > 0 && msg_have >= msg_need) {
                    // Emit one logical message line
                    int show_ts = (printed_any_lines_for_this_packet == 0);
                    print_msg_line(show_ts, ts, msg_buf, msg_need);
                    printed_any_lines_for_this_packet = 1;

                    // Reset for next message; keep running_status for possible running data stream
                    msg_have = 0;
                    msg_need = 0;
                }
            }
        }

        pkt = MIDIPacketNext(pkt);
    }
}

int main(void) {
    MIDIClientRef client = 0;
    MIDIPortRef inPort = 0;

    OSStatus st = MIDIClientCreate(CFSTR("midiecho-client"), NULL, NULL, &client);
    if (st != noErr) {
        fprintf(stderr, "MIDIClientCreate failed: %d\n", (int)st);
        return 1;
    }

    st = MIDIInputPortCreate(client, CFSTR("midiecho-in"), midi_read_proc, NULL, &inPort);
    if (st != noErr) {
        fprintf(stderr, "MIDIInputPortCreate failed: %d\n", (int)st);
        return 1;
    }

    ItemCount nsrc = MIDIGetNumberOfSources();
    if (nsrc == 0) {
        fprintf(stderr, "No MIDI sources found.\n");
        return 2;
    }

    printf("Found %lu MIDI source(s). Connecting...\n", (unsigned long)nsrc);
    for (ItemCount i = 0; i < nsrc; i++) {
        MIDIEndpointRef src = MIDIGetSource(i);
        if (!src) continue;

        CFStringRef name = NULL;
        MIDIObjectGetStringProperty(src, kMIDIPropertyName, &name);

        printf("  [%lu] ", (unsigned long)i);
        print_cfstring(name);
        printf("\n");
        if (name) CFRelease(name);

        st = MIDIPortConnectSource(inPort, src, NULL);
        if (st != noErr) {
            fprintf(stderr, "MIDIPortConnectSource(%lu) failed: %d\n",
                    (unsigned long)i, (int)st);
        }
    }

    printf("Echoing incoming MIDI... (Ctrl-C to quit)\n");
    CFRunLoopRun();

    MIDIPortDispose(inPort);
    MIDIClientDispose(client);
    return 0;
}
