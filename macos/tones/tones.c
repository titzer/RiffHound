// tones.c – Standalone macOS tone generator
// Generates sine, square, and triangle waves at a user-selected frequency.
//
// Keyboard:
//   Space  – play / stop
//   –      – decrease frequency by one semitone
//   +/=    – increase frequency by one semitone
//
// Build:
//   clang -x objective-c -fobjc-arc tones.c \
//       -framework Cocoa -framework AVFoundation -o Tones.app/Contents/MacOS/tones

#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

// ── Constants ─────────────────────────────────────────────────────────────
#define DEFAULT_FREQ    440.0
#define MIN_FREQ         10.0
#define MAX_FREQ      20000.0
#define SEMITONE_RATIO  1.0594630943592953   // 2^(1/12)
#define SAMPLE_RATE    44100.0

typedef enum { WAVE_SINE = 0, WAVE_SQUARE = 1, WAVE_TRIANGLE = 2 } WaveType;

// ── Shared state (main thread writes, audio render thread reads) ───────────
// All written with relaxed / release ordering; audio thread reads with
// acquire / relaxed — safe because these are independent scalar values.
static _Atomic double  s_frequency;   // Hz
static _Atomic int     s_wave_type;   // WaveType
static _Atomic int     s_playing;     // 0 = silent, 1 = generating

// ════════════════════════════════════════════════════════════════════════════
// TonesView — content view: dark background + colored border
// ════════════════════════════════════════════════════════════════════════════

@interface TonesView : NSView
@property (assign) BOOL playing;  // controls border color
@end

@implementation TonesView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }

// Click on the background → resign text-field focus back to this view.
- (void)mouseDown:(NSEvent *)event {
    [self.window makeFirstResponder:self];
    [super mouseDown:event];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect b = self.bounds;

    // ── Dark background ────────────────────────────────────────────────────
    [[NSColor colorWithWhite:0.10 alpha:1.0] setFill];
    NSRectFill(b);

    // ── Inner border: bright green when playing, near-black when stopped ──
    NSColor *borderColor = self.playing
        ? [NSColor colorWithRed:0.05 green:0.88 blue:0.20 alpha:1.0]
        : [NSColor colorWithWhite:0.14 alpha:1.0];

    [borderColor setStroke];
    NSBezierPath *border = [NSBezierPath
        bezierPathWithRect:NSInsetRect(b, 3.0, 3.0)];
    [border setLineWidth:4.5];
    [border stroke];
}

@end


// ════════════════════════════════════════════════════════════════════════════
// AppDelegate
// ════════════════════════════════════════════════════════════════════════════

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTextFieldDelegate>
@property (strong) NSWindow             *window;
@property (strong) TonesView            *tonesView;
@property (strong) NSSegmentedControl   *waveSelector;
@property (strong) NSTextField          *freqField;
@property (strong) NSTextField          *statusLabel;
@property (strong) AVAudioEngine        *audioEngine;
@property (strong) AVAudioSourceNode    *sourceNode;
@property (assign) BOOL                  isPlaying;
@property (assign) double                frequency;
@end

@implementation AppDelegate

// ── Frequency ──────────────────────────────────────────────────────────────

- (void)applyFrequency:(double)f {
    f = MAX(MIN_FREQ, MIN(MAX_FREQ, f));
    _frequency = f;
    atomic_store_explicit(&s_frequency, f, memory_order_relaxed);
    [self.freqField setStringValue:[NSString stringWithFormat:@"%.2f", f]];
}

- (void)shiftSemitone:(int)dir {
    double f = self.frequency;
    if (dir > 0) f *= SEMITONE_RATIO;
    else         f /= SEMITONE_RATIO;
    [self applyFrequency:f];
}

// ── Wave type ──────────────────────────────────────────────────────────────

- (void)applyWaveType:(WaveType)wt {
    atomic_store_explicit(&s_wave_type, (int)wt, memory_order_relaxed);
    [self.waveSelector setSelectedSegment:(NSInteger)wt];
}

- (void)waveSelectorChanged:(NSSegmentedControl *)sender {
    [self applyWaveType:(WaveType)sender.selectedSegment];
}

// ── Play / stop ─────────────────────────────────────────────────────────────

- (void)updatePlayingUI {
    self.tonesView.playing = self.isPlaying;
    [self.tonesView setNeedsDisplay:YES];
    if (self.isPlaying) {
        [self.statusLabel setStringValue:@"Playing"];
        [self.statusLabel setTextColor:[NSColor systemGreenColor]];
    } else {
        [self.statusLabel setStringValue:@"Stopped"];
        [self.statusLabel setTextColor:[NSColor colorWithWhite:0.50 alpha:1.0]];
    }
}

- (void)play {
    if (self.isPlaying) return;
    self.isPlaying = YES;
    atomic_store_explicit(&s_playing, 1, memory_order_release);
    // Engine should already be running from setupAudio; restart if needed.
    if (self.audioEngine && !self.audioEngine.isRunning) {
        [self.audioEngine prepare];
        NSError *err = nil;
        if (![self.audioEngine startAndReturnError:&err])
            NSLog(@"[Tones] engine start error: %@", err.localizedDescription);
    }
    [self updatePlayingUI];
}

- (void)stop {
    if (!self.isPlaying) return;
    self.isPlaying = NO;
    atomic_store_explicit(&s_playing, 0, memory_order_release);
    [self updatePlayingUI];
}

// ── NSTextFieldDelegate ────────────────────────────────────────────────────

- (void)controlTextDidEndEditing:(NSNotification *)note {
    NSTextField *f = [note object];
    if (f == self.freqField)
        [self applyFrequency:[[f stringValue] doubleValue]];
    [self.window makeFirstResponder:self.tonesView];
}

// ── Audio setup ────────────────────────────────────────────────────────────
// We start the AVAudioEngine immediately (outputting silence) so the first
// press of Space triggers sound with no startup latency.  The render block
// gates output through the s_playing atomic flag.

- (void)setupAudio {
    self.audioEngine = [[AVAudioEngine alloc] init];

    AVAudioFormat *fmt = [[AVAudioFormat alloc]
        initWithCommonFormat:AVAudioPCMFormatFloat32
                 sampleRate:SAMPLE_RATE
                   channels:1
                interleaved:NO];

    __block double phase = 0.0;   // phase accumulator [0, 1)

    self.sourceNode = [[AVAudioSourceNode alloc]
        initWithFormat:fmt
           renderBlock:^OSStatus(BOOL *isSilence,
                                 const AudioTimeStamp *ts,
                                 AVAudioFrameCount frameCount,
                                 AudioBufferList *outputData)
    {
        (void)ts;
        float *buf = (float *)outputData->mBuffers[0].mData;

        if (!atomic_load_explicit(&s_playing, memory_order_acquire)) {
            memset(buf, 0, sizeof(float) * frameCount);
            *isSilence = YES;
            return noErr;
        }

        double freq     = atomic_load_explicit(&s_frequency, memory_order_relaxed);
        int    wt       = atomic_load_explicit(&s_wave_type, memory_order_relaxed);
        double phaseInc = freq / SAMPLE_RATE;

        // Amplitude scaling: square waves are perceptually louder at equal peak.
        float scale = (wt == WAVE_SQUARE) ? 0.18f : 0.40f;

        for (AVAudioFrameCount i = 0; i < frameCount; i++) {
            double p = phase;
            float  samp;

            switch (wt) {
                case WAVE_SINE:
                    samp = (float)sin(2.0 * M_PI * p);
                    break;

                case WAVE_SQUARE:
                    samp = (p < 0.5) ? 1.0f : -1.0f;
                    break;

                default: /* WAVE_TRIANGLE */
                    if      (p < 0.25) samp = (float)(4.0 * p);
                    else if (p < 0.75) samp = (float)(2.0 - 4.0 * p);
                    else               samp = (float)(4.0 * p - 4.0);
                    break;
            }

            buf[i] = samp * scale;

            phase += phaseInc;
            if (phase >= 1.0) phase -= 1.0;
        }
        return noErr;
    }];

    [self.audioEngine attachNode:self.sourceNode];
    [self.audioEngine connect:self.sourceNode
                           to:self.audioEngine.mainMixerNode
                       format:fmt];
    [self.audioEngine prepare];

    NSError *err = nil;
    if (![self.audioEngine startAndReturnError:&err])
        NSLog(@"[Tones] initial engine start error: %@", err.localizedDescription);

    NSLog(@"[Tones] audio engine started (silent until Space is pressed)");
}

// ── Menus ──────────────────────────────────────────────────────────────────

- (void)menuAbout:(id)sender {
    (void)sender;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText     = @"Tone Generator";
    a.informativeText =
        @"Generates sine, square, and triangle waves.\n\n"
         "Keyboard shortcuts:\n"
         "  Space — play / stop\n"
         "  –     — decrease frequency one semitone\n"
         "  + / = — increase frequency one semitone\n\n"
         "Frequency range: 10 – 20,000 Hz.";
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

- (void)menuQuit:(id)sender { (void)sender; [NSApp terminate:nil]; }

- (void)buildMenus {
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
    [NSApp setMainMenu:bar];

    NSMenuItem *appMI = [[NSMenuItem alloc] initWithTitle:@""
                                                   action:nil
                                            keyEquivalent:@""];
    [bar addItem:appMI];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"App"];
    [appMI setSubmenu:appMenu];

    NSString *name = [[NSProcessInfo processInfo] processName];

    NSMenuItem *aboutMI = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithFormat:@"About %@", name]
               action:@selector(menuAbout:)
        keyEquivalent:@""];
    [aboutMI setTarget:self];
    [appMenu addItem:aboutMI];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *quitMI = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithFormat:@"Quit %@", name]
               action:@selector(menuQuit:)
        keyEquivalent:@"q"];
    [quitMI setTarget:self];
    [appMenu addItem:quitMI];
}

// ── UI assembly ────────────────────────────────────────────────────────────

- (void)buildWindowAndUI {
    NSRect contentRect = NSMakeRect(0, 0, 420, 130);

    self.window = [[NSWindow alloc]
        initWithContentRect:contentRect
                  styleMask:(NSWindowStyleMaskTitled      |
                             NSWindowStyleMaskClosable    |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [self.window setTitle:@"Tone Generator"];
    [self.window center];

    // Replace the default white content view with our custom dark one.
    TonesView *cv = [[TonesView alloc] initWithFrame:contentRect];
    [cv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [self.window setContentView:cv];
    self.tonesView = cv;

    CGFloat W  = contentRect.size.width;
    CGFloat bh = 28;    // standard control height
    CGFloat gap = 8;

    // ── Row 1 (bottom): keyboard-shortcut hint ─────────────────────────────
    CGFloat hintY = 12;
    NSTextField *hint = [[NSTextField alloc]
        initWithFrame:NSMakeRect(14, hintY, W - 28, 16)];
    [hint setStringValue:@"Space: play/stop    –: semitone ↓    + / =: semitone ↑"];
    [hint setEditable:NO]; [hint setBezeled:NO]; [hint setDrawsBackground:NO];
    [hint setFont:[NSFont systemFontOfSize:10]];
    [hint setTextColor:[NSColor colorWithWhite:0.38 alpha:1.0]];
    [cv addSubview:hint];

    // ── Row 2: frequency control ───────────────────────────────────────────
    CGFloat row2Y = hintY + 16 + gap;
    CGFloat x     = 14;

    NSTextField *freqLbl = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, row2Y + 3, 44, bh)];
    [freqLbl setStringValue:@"Freq:"];
    [freqLbl setEditable:NO]; [freqLbl setBezeled:NO]; [freqLbl setDrawsBackground:NO];
    [freqLbl setFont:[NSFont systemFontOfSize:13]];
    [freqLbl setTextColor:[NSColor colorWithWhite:0.68 alpha:1.0]];
    [cv addSubview:freqLbl];
    x += 46;

    self.freqField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, row2Y, 88, bh)];
    [self.freqField setStringValue:@"440.00"];
    [self.freqField setEditable:YES];
    [self.freqField setAlignment:NSTextAlignmentCenter];
    [self.freqField setFont:
        [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightRegular]];
    [self.freqField setDelegate:self];
    [cv addSubview:self.freqField];
    x += 92;

    NSTextField *hzLbl = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, row2Y + 3, 28, bh)];
    [hzLbl setStringValue:@"Hz"];
    [hzLbl setEditable:NO]; [hzLbl setBezeled:NO]; [hzLbl setDrawsBackground:NO];
    [hzLbl setFont:[NSFont systemFontOfSize:13]];
    [hzLbl setTextColor:[NSColor colorWithWhite:0.68 alpha:1.0]];
    [cv addSubview:hzLbl];

    // ── Row 3: waveform selector + status label ────────────────────────────
    CGFloat row3Y = row2Y + bh + gap;
    x = 14;

    NSSegmentedControl *seg = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(x, row3Y, 222, bh)];
    [seg setSegmentCount:3];
    [seg setLabel:@"Sine"     forSegment:0];
    [seg setLabel:@"Square"   forSegment:1];
    [seg setLabel:@"Triangle" forSegment:2];
    [seg setSelectedSegment:0];
    [seg setSegmentStyle:NSSegmentStyleRounded];
    [seg setTarget:self];
    [seg setAction:@selector(waveSelectorChanged:)];
    [cv addSubview:seg];
    self.waveSelector = seg;
    x += 230;

    self.statusLabel = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, row3Y + 2, W - x - 14, bh)];
    [self.statusLabel setEditable:NO];
    [self.statusLabel setBezeled:NO];
    [self.statusLabel setDrawsBackground:NO];
    [self.statusLabel setFont:
        [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold]];
    [self.statusLabel setAlignment:NSTextAlignmentLeft];
    [cv addSubview:self.statusLabel];

    // ── Initial state ──────────────────────────────────────────────────────
    atomic_store_explicit(&s_frequency, DEFAULT_FREQ,  memory_order_relaxed);
    atomic_store_explicit(&s_wave_type, (int)WAVE_SINE, memory_order_relaxed);
    atomic_store_explicit(&s_playing,   0,             memory_order_relaxed);

    _frequency    = DEFAULT_FREQ;
    self.isPlaying = NO;
    [self updatePlayingUI];

    // ── Local key monitor ──────────────────────────────────────────────────
    __weak AppDelegate *ws = self;
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *ev) {
        AppDelegate *s = ws;
        if (!s) return ev;

        // Ignore Cmd / Ctrl / Opt key combos (let the system handle them).
        NSEventModifierFlags held = [ev modifierFlags] &
            (NSEventModifierFlagCommand |
             NSEventModifierFlagControl |
             NSEventModifierFlagOption);
        if (held) return ev;

        NSString *ch = [ev characters];

        // Space always toggles play / stop, even while editing the freq field.
        if ([ch isEqualToString:@" "]) {
            if (s.isPlaying) [s stop]; else [s play];
            return nil;   // consume event
        }

        // – and + only shift semitones when the freq field is NOT being edited,
        // so the user can still type a '-' sign or press '=' in the text field.
        BOOL editingText = [[s.window firstResponder]
            isKindOfClass:[NSTextView class]];
        if (!editingText) {
            if ([ch isEqualToString:@"-"]) {
                [s shiftSemitone:-1];
                return nil;
            }
            if ([ch isEqualToString:@"+"] || [ch isEqualToString:@"="]) {
                [s shiftSemitone:+1];
                return nil;
            }
        }

        return ev;
    }];

    [self.window setInitialFirstResponder:cv];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:cv];
}

// ── Application lifecycle ──────────────────────────────────────────────────

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    [self buildMenus];
    [self buildWindowAndUI];
    [NSApp activateIgnoringOtherApps:YES];
    [self setupAudio];   // engine starts silently; press Space to play
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a {
    (void)a;
    return YES;
}

@end


// ════════════════════════════════════════════════════════════════════════════
// Entry point
// ════════════════════════════════════════════════════════════════════════════

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
