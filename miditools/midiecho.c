#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static void midi_read_proc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon) {
    (void)refCon;
    (void)connRefCon;

    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        // pkt->timeStamp is a CoreMIDI host timestamp (mach time units), not necessarily wall-clock.
        // Still useful for relative timing/jitter.
        printf("%" PRIu64 " : ", (uint64_t)pkt->timeStamp);

        for (UInt16 j = 0; j < pkt->length; j++) {
            printf("%02X", pkt->data[j]);
            if (j + 1 < pkt->length) putchar(' ');
        }
        putchar('\n');
        fflush(stdout);

        pkt = MIDIPacketNext(pkt);
    }
}

static void print_cfstring(CFStringRef s) {
    if (!s) { printf("(null)"); return; }
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) {
        printf("%s", buf);
    } else {
        printf("(unprintable)");
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
        if (src == 0) continue;

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
    // CoreMIDI delivers callbacks on the run loop.
    CFRunLoopRun();

    // Unreachable in normal use, but kept for completeness.
    MIDIPortDispose(inPort);
    MIDIClientDispose(client);
    return 0;
}
