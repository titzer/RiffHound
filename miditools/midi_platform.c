#include "midi_platform.h"
#include "midi_stream.h"
#include <stdio.h>

// ── macOS / CoreMIDI ──────────────────────────────────────────────────────────
#ifdef __APPLE__

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

typedef struct {
    midi_msg_handler_t  handler;
    void               *userdata;
    midi_parse_state_t  parse_state;
} midi_run_ctx_t;

static void midi_read_proc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon) {
    (void)connRefCon;
    midi_run_ctx_t *ctx = (midi_run_ctx_t *)refCon;
    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        midi_parse_bytes(&ctx->parse_state,
                         pkt->data, (int)pkt->length, (uint64_t)pkt->timeStamp,
                         ctx->handler, ctx->userdata);
        pkt = MIDIPacketNext(pkt);
    }
}

static void print_cfstring(CFStringRef s) {
    if (!s) { printf("(null)"); return; }
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
        printf("%s", buf);
    else
        printf("(unprintable)");
}

int midi_run(midi_msg_handler_t handler, void *userdata) {
    static midi_run_ctx_t ctx;
    midi_parse_state_init(&ctx.parse_state);
    ctx.handler  = handler;
    ctx.userdata = userdata;

    MIDIClientRef client = 0;
    MIDIPortRef   inPort = 0;

    OSStatus st = MIDIClientCreate(CFSTR("midi-client"), NULL, NULL, &client);
    if (st != noErr) {
        fprintf(stderr, "MIDIClientCreate failed: %d\n", (int)st);
        return 1;
    }

    st = MIDIInputPortCreate(client, CFSTR("midi-in"), midi_read_proc, &ctx, &inPort);
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
        if (st != noErr)
            fprintf(stderr, "MIDIPortConnectSource(%lu) failed: %d\n",
                    (unsigned long)i, (int)st);
    }

    printf("Listening for MIDI... (Ctrl-C to quit)\n");
    CFRunLoopRun();

    MIDIPortDispose(inPort);
    MIDIClientDispose(client);
    return 0;
}

#endif // __APPLE__

// ── Linux / ALSA (stub) ───────────────────────────────────────────────────────
#ifdef __linux__

int midi_run(midi_msg_handler_t handler, void *userdata) {
    (void)handler;
    (void)userdata;
    fprintf(stderr, "Linux MIDI not yet implemented.\n");
    return 1;
}

#endif // __linux__
