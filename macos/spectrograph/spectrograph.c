#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#include "fft.h"
#include <time.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>

#define DEFAULT_DISPLAY_SECS 5
#define DEFAULT_DISPLAY_SECS_STR "5"
#define DEFAULT_MAX_FREQ 8000
#define DEFAULT_MAX_FREQ_STR "8000"

#define DEFAULT_FPS 60

// ── Monotonic clock ──────────────────────────────────────────────────────────
// Used for diagnostic timestamps written from the audio thread.
static double monotonic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ── Diagnostic ring buffers ──────────────────────────────────────────────────
// Single-producer (audio thread), single-consumer (main thread).
// Power-of-2 size allows masking instead of modulo.
// Protocol: producer writes data, then increments index with release ordering.
//           consumer reads index with acquire ordering, then reads data.
#define DIAG_BUF 8192u   // 8192 entries ≈ 190 s at 43 FFT frames/s (>max 99 s window)

static volatile double   diag_audio_ts[DIAG_BUF]; // tap callback timestamps
static _Atomic uint32_t  diag_audio_idx;           // monotonic write count

static volatile double   diag_fft_ts[DIAG_BUF];   // FFT frame timestamps
static _Atomic uint32_t  diag_fft_idx;             // monotonic write count

// ── Spectrogram frame ring buffer ────────────────────────────────────────────
// Written from the audio tap thread; read from the main/draw thread.
// Same SPSC protocol as the diag buffers above.
#define SGRAM_MAX_FRAMES 4096u   // ~47 s at 86 FFT/s (hop=512, sr=44100); enough for default 5-s window
#define SGRAM_MAX_BINS   2049    // fft_size/2+1 for default fft_size=4096

static float            s_sgram_buf[SGRAM_MAX_FRAMES][SGRAM_MAX_BINS];
static double           s_sgram_ts[SGRAM_MAX_FRAMES];
static _Atomic uint32_t s_sgram_write;   // monotonic write count

// Diagnostic flag; flipped with SHIFT + D
static volatile int diagnose = 1;

// ── Display clock ─────────────────────────────────────────────────────────────
// Frame timestamps are raw monotonic_now() values.  To keep historical data
// stationary across stop/start cycles we maintain a display clock that only
// advances while playing:
//
//   display_now  =  monotonic_now() - s_pause_offset   (while playing)
//   display_now  =  s_freeze_time                       (while stopped)
//
// On stop:  s_freeze_time = monotonic_now() - s_pause_offset
//           s_stop_real   = monotonic_now()
// On play:  s_pause_offset += monotonic_now() - s_stop_real
//           s_freeze_time  = 0.0
//
// Because s_pause_offset grows by exactly the pause duration each time,
// display_now continues smoothly from the freeze point on resume.
static double s_pause_offset  = 0.0;  // total accumulated pause time (seconds)
static double s_stop_real     = 0.0;  // monotonic_now() value at most recent stop
static double s_freeze_time   = 0.0;  // display clock value when stopped (0 = live)

// ── Display configuration (hookable to UI controls later) ────────────────────
// dB floor: audio level that maps to the darkest (black) color.
// dB range: span above the floor that maps to full brightness.
// Tradeoff: wider range shows quieter sounds but compresses loud ones.
// Blending: each pixel column always shows the newest FFT frame that falls in
//   that time slot ("newest wins").  Temporal blending (averaging overlapping
//   frames into the same column) would smooth noise but blur transients; given
//   the Hann window already smears ~93 ms of audio per frame, extra blending
//   adds little benefit and hurts temporal sharpness — so we don't do it.
static float s_db_floor = -80.0f;   // dB level → black  (raise to hide quiet noise)
static float s_db_range =  80.0f;   // dB span  → white  (lower for more contrast)

// Six-stop thermal heatmap: black → purple → blue → cyan → green → yellow → red
static void sgram_heatmap(float t, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (t <= 0.0f) { *r = 0;   *g = 0;   *b = 0;   return; }
    if (t >= 1.0f) { *r = 255; *g = 40;  *b = 10;  return; }
    float s;
    if      (t < 1.f/6) { s=(t      )*6; *r=(uint8_t)(80*s);       *g=0;                    *b=(uint8_t)(160*s);          }
    else if (t < 2.f/6) { s=(t-1.f/6)*6; *r=(uint8_t)(80*(1-s));   *g=0;                    *b=(uint8_t)(160+95*s);       }
    else if (t < 3.f/6) { s=(t-2.f/6)*6; *r=0;                     *g=(uint8_t)(220*s);     *b=(uint8_t)(255*(1-s));      }
    else if (t < 4.f/6) { s=(t-3.f/6)*6; *r=(uint8_t)(80*s);       *g=(uint8_t)(220+35*s);  *b=0;                         }
    else if (t < 5.f/6) { s=(t-4.f/6)*6; *r=(uint8_t)(80+175*s);   *g=255;                  *b=0;                         }
    else                { s=(t-5.f/6)*6; *r=255;                    *g=(uint8_t)(255*(1-s*0.8f)); *b=0;                    }
}

// ════════════════════════════════════════════════════════════════════════════
// SpectrogramView  –  draws axes, grid, and diagnostic overlay
// ════════════════════════════════════════════════════════════════════════════

@interface SpectrogramView : NSView
@property (assign) NSInteger displaySeconds;   // 2–99,          default 5
@property (assign) CGFloat   maxFrequency;     // 1000–20000 Hz, default 8000
@end

@implementation SpectrogramView

static const CGFloat kLeftMargin   = 30.0;    // px for Y-axis label strip
static const CGFloat kFreqInterval = 1000.0;  // Hz between horizontal grid lines

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _displaySeconds = DEFAULT_DISPLAY_SECS;
        _maxFrequency   = DEFAULT_MAX_FREQ;
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;

    // ── Shared quantities ─────────────────────────────────────────────────
    static uint64_t s_draw_count = 0;
    s_draw_count++;

    NSRect  b  = self.bounds;
    CGFloat gX = kLeftMargin;
    CGFloat gY = 0.0;
    CGFloat gW = b.size.width  - gX;
    CGFloat gH = b.size.height;

    double  now    = (s_freeze_time > 0.0) ? s_freeze_time
                                           : (monotonic_now() - s_pause_offset);
    double  winDur = (double)self.displaySeconds;
    CGFloat maxFreq = self.maxFrequency;

    // ── 1. Backgrounds ────────────────────────────────────────────────────
    [[NSColor colorWithWhite:0.06 alpha:1.0] setFill];
    NSRectFill(b);
    [[NSColor blackColor] setFill];
    NSRectFill(NSMakeRect(gX, gY, gW, gH));

    // ── 2. Spectrogram (pixel buffer → bitmap → blit to graph area) ───────
    // Drawn before the grid so axis lines overlay the data.
    if (fft_freq_resolution > 0.0) {
        int pixW = MAX(1, (int)ceil(gW));
        int pixH = MAX(1, (int)ceil(gH));

        NSBitmapImageRep *bmp = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:pixW
                          pixelsHigh:pixH
                       bitsPerSample:8
                     samplesPerPixel:3
                            hasAlpha:NO
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:0
                        bitsPerPixel:0];
        unsigned char *data = [bmp bitmapData];
        NSInteger      bpr  = [bmp bytesPerRow];

        if (data) {
            memset(data, 0, (size_t)pixH * (size_t)bpr);  // fill black

            uint32_t head = atomic_load_explicit(&s_sgram_write, memory_order_acquire);
            uint32_t n    = (head < SGRAM_MAX_FRAMES) ? head : SGRAM_MAX_FRAMES;

            int maxBin = (int)((double)maxFreq / fft_freq_resolution);
            if (maxBin > fft_bin_count - 1) maxBin = fft_bin_count - 1;
            if (maxBin > SGRAM_MAX_BINS  - 1) maxBin = SGRAM_MAX_BINS - 1;
            if (maxBin < 1) maxBin = 1;

            // Seed xFilled from the newest in-window frame, not from pixW.
            // This prevents smearing stale data rightward into silent periods
            // (e.g. after stop/start: the newest pre-pause frame would otherwise
            // be painted all the way to the right edge, covering the pause gap).
            // Between consecutive live frames the fill still closes timing jitter.
            int xFilled = 0;
            if (n > 0) {
                uint32_t slot0 = (head - 1) & (SGRAM_MAX_FRAMES - 1);
                double age0 = now - s_sgram_ts[slot0];
                if (age0 >= 0.0 && age0 <= winDur) {
                    int x0 = (int)((1.0 - age0 / winDur) * (double)(pixW - 1) + 0.5);
                    if (x0 < 0) x0 = 0;
                    xFilled = x0 + 1;
                    if (xFilled > pixW) xFilled = pixW;
                }
            }

            // Iterate from newest frame to oldest, filling contiguous pixel-column
            // ranges so that no columns are left black due to sparse FFT timing.
            for (uint32_t j = 0; j < n; j++) {
                uint32_t slot = (head - 1 - j) & (SGRAM_MAX_FRAMES - 1);
                double age = now - s_sgram_ts[slot];
                if (age < 0.0) continue;   // future timestamp: skip, don't abort loop
                if (age > winDur) break;   // too old: all further frames are older too

                int xPix = (int)((1.0 - age / winDur) * (double)(pixW - 1) + 0.5);
                if (xPix >= xFilled) continue;  // already covered by a newer frame
                if (xPix < 0) xPix = 0;

                // Fill columns [xPix, xFilled-1] with this frame's data.
                int xRight = xFilled - 1;
                if (xRight >= pixW) xRight = pixW - 1;

                for (int bin = 1; bin <= maxBin; bin++) {
                    float mag = s_sgram_buf[slot][bin];
                    float dB  = 20.0f * log10f(mag + 1e-9f);
                    float t   = (dB - s_db_floor) / s_db_range;
                    if (t <= 0.0f) continue;
                    if (t  > 1.0f) t = 1.0f;

                    // row 0 = top of bitmap = highest displayed frequency
                    double freq = (double)bin * fft_freq_resolution;
                    int row = (int)((1.0 - freq / (double)maxFreq) * (double)(pixH - 1) + 0.5);
                    if (row < 0 || row >= pixH) continue;

                    uint8_t r, g, bv;
                    sgram_heatmap(t, &r, &g, &bv);

                    unsigned char *rowBase = data + (size_t)row * (size_t)bpr;
                    for (int xCol = xPix; xCol <= xRight; xCol++) {
                        unsigned char *px = rowBase + (size_t)xCol * 3;
                        px[0] = r; px[1] = g; px[2] = bv;
                    }
                }

                xFilled = xPix;
                if (xFilled <= 0) break;
            }

            [[NSGraphicsContext currentContext]
                setImageInterpolation:NSImageInterpolationNone];
            [bmp drawInRect:NSMakeRect(gX, gY, gW, gH)];
        }
    }

    NSDictionary *yLblAttrs = @{
        NSFontAttributeName:
            [NSFont monospacedDigitSystemFontOfSize:9 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.65 alpha:1.0],
    };
    NSDictionary *xLblAttrs = @{
        NSFontAttributeName:
            [NSFont monospacedDigitSystemFontOfSize:9 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.55 alpha:1.0],
    };
    NSColor *gridColor = [NSColor colorWithWhite:0.22 alpha:1.0];

    // ── 3. Horizontal grid lines (frequency) + Y-axis labels in margin ────
    for (CGFloat freq = kFreqInterval; freq < maxFreq; freq += kFreqInterval) {
        CGFloat yPos = gY + (freq / maxFreq) * gH;

        [gridColor setStroke];
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line setLineWidth:0.5];
        [line moveToPoint:NSMakePoint(gX,      yPos)];
        [line lineToPoint:NSMakePoint(gX + gW, yPos)];
        [line stroke];

        NSString *lbl = (freq >= 1000.0)
            ? [NSString stringWithFormat:@"%.0fk", freq / 1000.0]
            : [NSString stringWithFormat:@"%.0f",  freq];
        NSSize ls = [lbl sizeWithAttributes:yLblAttrs];
        [lbl drawAtPoint:NSMakePoint(gX - ls.width - 4, yPos - ls.height * 0.5)
          withAttributes:yLblAttrs];
    }

    // ── 4. Vertical grid lines (time) + labels inside graph ───────────────
    NSInteger secs = self.displaySeconds;
    for (NSInteger i = 0; i <= secs; i++) {
        CGFloat xPos = gX + gW - (CGFloat)i / (CGFloat)secs * gW;

        [gridColor setStroke];
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line setLineWidth:0.5];
        [line moveToPoint:NSMakePoint(xPos, gY)];
        [line lineToPoint:NSMakePoint(xPos, gY + gH)];
        [line stroke];

        NSString *lbl = (i == 0)
            ? @"0"
            : [NSString stringWithFormat:@"-%ld", (long)i];
        NSSize  ls = [lbl sizeWithAttributes:xLblAttrs];
        CGFloat lx = (i == 0) ? xPos - ls.width - 2 : xPos + 2;
        [lbl drawAtPoint:NSMakePoint(lx, gY + 4) withAttributes:xLblAttrs];
    }

    // ── 5. Diagnostic markers ─────────────────────────────────────────────


    // Animated red dot — bounces left↔right at redraw fps (proves timer + drawRect work).
    // Period = DEFAULT_FPS * 4 frames (2 s each way), so speed is FPS-independent.
    if (diagnose) {
        uint64_t period = (uint64_t)(DEFAULT_FPS * 4);
        uint64_t half   = (uint64_t)(DEFAULT_FPS * 2);
        uint64_t step   = s_draw_count % period;
        CGFloat  frac   = (step < half) ? (CGFloat)step / (CGFloat)(half - 1)
                                        : (CGFloat)(period - step) / (CGFloat)(half - 1);
        CGFloat dotX = gX + 8.0 + frac * (gW - 16.0);
        CGFloat dotY = gY + gH - 16.0;
        [[NSColor systemRedColor] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(dotX - 5.0, dotY - 5.0, 10.0, 10.0)] fill];
    }

    // FFT frame markers: green diamonds, row just below the red dot.
    if (diagnose) {
        uint32_t head = atomic_load_explicit(&diag_fft_idx, memory_order_acquire);
        uint32_t n    = (head < DIAG_BUF) ? head : DIAG_BUF;
        [[NSColor colorWithRed:0.2 green:1.0 blue:0.35 alpha:0.9] setFill];
        for (uint32_t j = 0; j < n; j++) {
            uint32_t slot = (head - 1 - j) & (DIAG_BUF - 1);
            double age = now - diag_fft_ts[slot];
            if (age < 0.0) continue;
            if (age > winDur) break;
            CGFloat xp = gX + gW - (CGFloat)(age / winDur) * gW;
            CGFloat yp = gY + gH - 32.0;
            NSBezierPath *d = [NSBezierPath bezierPath];
            [d moveToPoint:NSMakePoint(xp,       yp + 4.0)];
            [d lineToPoint:NSMakePoint(xp + 4.0, yp)];
            [d lineToPoint:NSMakePoint(xp,       yp - 4.0)];
            [d lineToPoint:NSMakePoint(xp - 4.0, yp)];
            [d closePath]; [d fill];
        }
    }

    // Audio callback markers: cyan dots, below the FFT diamonds.
    if (diagnose) {
        uint32_t head = atomic_load_explicit(&diag_audio_idx, memory_order_acquire);
        uint32_t n    = (head < DIAG_BUF) ? head : DIAG_BUF;
        [[NSColor colorWithRed:0.3 green:0.9 blue:1.0 alpha:0.7] setFill];
        for (uint32_t j = 0; j < n; j++) {
            uint32_t slot = (head - 1 - j) & (DIAG_BUF - 1);
            double age = now - diag_audio_ts[slot];
            if (age < 0.0) continue;
            if (age > winDur) break;
            CGFloat xp = gX + gW - (CGFloat)(age / winDur) * gW;
            CGFloat yp = gY + gH - 44.0;
            [[NSBezierPath bezierPathWithOvalInRect:
                NSMakeRect(xp - 2.5, yp - 2.5, 5.0, 5.0)] fill];
        }
    }

    // ── 6. Diagnostic text (centred, semi-transparent backing) ────────────
    if (diagnose) {
        NSDictionary *diagAttrs = @{
            NSFontAttributeName:
                [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [NSColor whiteColor],
        };
        uint32_t aCnt = atomic_load_explicit(&diag_audio_idx, memory_order_relaxed);
        uint32_t fCnt = atomic_load_explicit(&diag_fft_idx,   memory_order_relaxed);
        NSString *diagLine = [NSString stringWithFormat:
            @"draw#%llu  fft:%u  cb:%u  sr:%.0fHz  N:%d  hop:%d  bins:%d  df:%.2fHz",
            s_draw_count, fCnt, aCnt,
            fft_sample_rate, fft_size, fft_hop_size, fft_bin_count, fft_freq_resolution];
        NSSize  ts = [diagLine sizeWithAttributes:diagAttrs];
        //        CGFloat tx = gX + (gW - ts.width)  * 0.5;
        //        CGFloat ty = gY + (gH - ts.height) * 0.5;
        CGFloat tx = gX + 5;
        CGFloat ty = gY + gH - ts.height;
        [[NSColor colorWithWhite:0.0 alpha:0.6] setFill];
        NSRectFillUsingOperation(
            NSInsetRect(NSMakeRect(tx - 6, ty - 4, ts.width + 12, ts.height + 8), -1, -1),
            NSCompositingOperationSourceOver);
        [diagLine drawAtPoint:NSMakePoint(tx, ty) withAttributes:diagAttrs];
    }

    // ── 7. Border (drawn last, sits on top of everything) ─────────────────
    [[NSColor colorWithWhite:0.45 alpha:1.0] setStroke];
    NSBezierPath *border = [NSBezierPath bezierPathWithRect:
        NSInsetRect(NSMakeRect(gX, gY, gW, gH), 0.5, 0.5)];
    [border setLineWidth:1.0];
    [border stroke];
}

@end

// ════════════════════════════════════════════════════════════════════════════
// AppDelegate
// ════════════════════════════════════════════════════════════════════════════

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTextFieldDelegate, NSWindowDelegate>
@property (strong) NSWindow        *window;
@property (strong) NSTextField     *label;           // event feedback (flexible)
@property (strong) NSTextField     *statusLabel;     // Running / Stopped
@property (strong) NSButton        *startButton;
@property (strong) NSButton        *stopButton;
@property (strong) NSButton        *fullscreenButton;
@property (strong) SpectrogramView *spectrogramView;
@property (strong) NSTextField     *secsField;       // horizontal axis duration
@property (strong) NSTextField     *maxHzField;      // vertical axis max frequency
@property (strong) AVAudioEngine   *audioEngine;
@property (strong) id               audioConfigObserver; // AVAudioEngineConfigurationChangeNotification
@property (assign) BOOL             isRunning;
@property (assign) NSInteger        displaySeconds;  // 2–99
@property (assign) CGFloat          maxFrequency;    // 1000–20000 Hz
@end

@implementation AppDelegate

- (void)setStatus:(NSString *)text { [self.label setStringValue:text]; }

- (void)updatePlaybackUI {
    if (self.isRunning) {
        [self.statusLabel setStringValue:@"Running"];
        [self.statusLabel setTextColor:[NSColor systemGreenColor]];
        [self.startButton setEnabled:NO];
        [self.stopButton  setEnabled:YES];
    } else {
        [self.statusLabel setStringValue:@"Stopped"];
        [self.statusLabel setTextColor:[NSColor secondaryLabelColor]];
        [self.startButton setEnabled:YES];
        [self.stopButton  setEnabled:NO];
    }
}

- (void)play {
    if (self.isRunning) return;
    if (s_stop_real > 0.0)
        s_pause_offset += monotonic_now() - s_stop_real;  // skip over pause duration
    s_stop_real   = 0.0;
    s_freeze_time = 0.0;   // resume display clock: monotonic_now() - s_pause_offset
    self.isRunning = YES;
    [self updatePlaybackUI];
    if (self.audioEngine && !self.audioEngine.isRunning) {
        [self.audioEngine prepare];   // wire the graph before starting
        NSError *err = nil;
        if (![self.audioEngine startAndReturnError:&err]) {
            [self setStatus:[NSString stringWithFormat:@"Audio error: %@",
                             err.localizedDescription]];
            self.isRunning = NO;
            [self updatePlaybackUI];
        }
    }
}

- (void)stop {
    if (!self.isRunning) return;
    s_stop_real   = monotonic_now();
    s_freeze_time = s_stop_real - s_pause_offset;  // freeze display clock here
    self.isRunning = NO;
    [self updatePlaybackUI];
    if (self.audioEngine.isRunning)
        [self.audioEngine stop];
}

// ── Audio setup ────────────────────────────────────────────────────────────

- (void)setupAudio {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!granted) { [self setStatus:@"Microphone access denied."]; return; }
            [self buildAudioEngine];
        });
    }];
}

- (void)buildAudioEngine {
    // Remove any previous config-change observer before releasing the old engine.
    if (self.audioConfigObserver)
        [[NSNotificationCenter defaultCenter] removeObserver:self.audioConfigObserver];

    self.audioEngine = [[AVAudioEngine alloc] init];
    AVAudioInputNode *inputNode = [self.audioEngine inputNode];

    // Query the hardware format WITHOUT calling prepare first.
    // Calling prepare before the tap is installed can produce a 0 Hz format
    // on macOS 14+, which makes the tap silently deliver no buffers.
    // If the format still comes back invalid, fall back to 48 kHz float32 mono
    // (universally supported; the engine will SRC from the hardware rate).
    AVAudioFormat *tapFmt = [inputNode outputFormatForBus:0];
    if (tapFmt.sampleRate <= 0.0 || tapFmt.channelCount == 0) {
        tapFmt = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                 sampleRate:48000.0
                                                   channels:1
                                                interleaved:NO];
    }

    // Retune FFT to match whichever sample rate we ended up with.
    fft_sample_rate = tapFmt.sampleRate;
    fft_reinit();

    // Install the analysis tap.
    // The block is called on a real-time audio thread — no Obj-C allocation,
    // no locks, only atomic ring-buffer writes.
    [inputNode installTapOnBus:0
                    bufferSize:1024
                        format:tapFmt
                         block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
        (void)when;

        double now = monotonic_now();

        // Record every tap invocation, even if the buffer is empty/wrong format.
        uint32_t aslot = atomic_load_explicit(&diag_audio_idx, memory_order_relaxed)
                         & (DIAG_BUF - 1);
        diag_audio_ts[aslot] = now;
        uint32_t newIdx = atomic_fetch_add_explicit(&diag_audio_idx, 1, memory_order_release) + 1;

        // Log the first 3 tap calls so we know the audio thread is alive.
        // NSLog is safe to call here — it is not realtime-safe, but these
        // first-call logs only happen at startup and won't glitch audio.
        if (newIdx <= 3) {
            NSLog(@"[Spectrograph] tap#%u: frames=%u floatData=%s",
                  newIdx, (unsigned)buf.frameLength,
                  buf.floatChannelData ? "ok" : "NULL");
        }

        if (!buf.floatChannelData) return;  // format mismatch — count still recorded above

        // Drive fft_push() in hop-sized chunks so EVERY FFT frame is captured.
        // fft_push() returns only the last frame computed per call, so passing the
        // entire buffer at once silently drops all but the final frame (e.g. an
        // 4096-sample buffer at hop=512 would produce 8 FFTs but only 1 would be
        // stored).  Chunking at hop_size guarantees at most one frame per call.
        // Each frame gets a timestamp proportional to its position in the buffer
        // so that consecutive frames spread across distinct pixel columns.
        const float *pcm  = buf.floatChannelData[0];
        int total    = (int)buf.frameLength;
        int consumed = 0;
        int nBins    = (fft_bin_count < SGRAM_MAX_BINS) ? fft_bin_count : SGRAM_MAX_BINS;

        while (consumed < total) {
            int chunk = total - consumed;
            if (chunk > fft_hop_size) chunk = fft_hop_size;

            const float *mags = fft_push(pcm + consumed, chunk);
            consumed += chunk;

            if (mags) {
                // Timestamp in display-clock time.
                // `now` (raw monotonic) minus the intra-buffer offset gives the
                // raw capture time of this frame's most recent sample.
                // Subtracting s_pause_offset converts it to display-clock time so
                // that ages (display_now - frame_ts) are correct after stop/start
                // cycles.  s_pause_offset is always updated before the engine
                // starts, so it is stable for the lifetime of this tap callback.
                double frame_ts = now
                                  - (double)(total - consumed) / fft_sample_rate
                                  - s_pause_offset;

                uint32_t sfslot = atomic_load_explicit(&s_sgram_write, memory_order_relaxed)
                                  & (SGRAM_MAX_FRAMES - 1);
                memcpy(s_sgram_buf[sfslot], mags, (size_t)nBins * sizeof(float));
                s_sgram_ts[sfslot] = frame_ts;
                atomic_fetch_add_explicit(&s_sgram_write, 1, memory_order_release);

                uint32_t fslot = atomic_load_explicit(&diag_fft_idx, memory_order_relaxed)
                                 & (DIAG_BUF - 1);
                diag_fft_ts[fslot] = frame_ts;
                atomic_fetch_add_explicit(&diag_fft_idx, 1, memory_order_release);
            }
        }
    }];

    NSLog(@"[Spectrograph] tap installed: %.0f Hz, %u ch, interleaved=%d",
          tapFmt.sampleRate, (unsigned)tapFmt.channelCount, (int)tapFmt.isInterleaved);
    [self setStatus:[NSString stringWithFormat:@"Audio ready (%.0f Hz, %u ch).",
                    tapFmt.sampleRate, (unsigned)tapFmt.channelCount]];

    // Rebuild the engine automatically if the audio hardware configuration changes
    // (e.g. another app takes the mic at a different sample rate, a device is
    // plugged/unplugged).  macOS stops AVAudioEngine and posts this notification;
    // without handling it the tap silently stops delivering buffers.
    // The observer is keyed to this specific engine instance and is removed at
    // the top of the next buildAudioEngine call, so observers don't accumulate.
    __weak AppDelegate *weakSelf = self;
    self.audioConfigObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioEngineConfigurationChangeNotification
                    object:self.audioEngine
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
        (void)note;
        AppDelegate *s = weakSelf;
        if (!s) return;
        NSLog(@"[Spectrograph] audio config changed; rebuilding engine");
        [s buildAudioEngine];
    }];

    // Auto-start if the user pressed Start while we were waiting for permission.
    if (self.isRunning) {
        [self.audioEngine prepare];
        NSError *err = nil;
        if (![self.audioEngine startAndReturnError:&err])
            [self setStatus:[NSString stringWithFormat:@"Audio error: %@",
                             err.localizedDescription]];
    }
}

- (void)playClicked:(id)sender       { (void)sender; [self play]; }
- (void)stopClicked:(id)sender       { (void)sender; [self stop]; }
- (void)fullscreenClicked:(id)sender { (void)sender; [self.window toggleFullScreen:nil]; }

- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; [self.fullscreenButton setTitle:@"Exit Full"]; }
- (void)windowDidExitFullScreen:(NSNotification *)n  { (void)n; [self.fullscreenButton setTitle:@"Full"]; }

- (void)buttonClicked:(id)sender {
    [self setStatus:[NSString stringWithFormat:@"Clicked: %@", [sender title]]];
}

- (void)menuSayHello:(id)sender {
    (void)sender;
    [self setStatus:@"Menu: Hello 👋"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText     = @"Realtime Audio Spectrograph";
    a.informativeText = @"This alert was triggered from the menu.";
    [a addButtonWithTitle:@"OK"]; [a runModal];
}

- (void)menuAbout:(id)sender {
    (void)sender;
    [self setStatus:@"Menu: About"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText     = @"About Spectrograph";
    a.informativeText = @"Single-file Cocoa app (built from a .c file compiled as Objective-C).";
    [a addButtonWithTitle:@"Nice"]; [a runModal];
}

- (void)menuQuit:(id)sender { (void)sender; [NSApp terminate:nil]; }

static void AddMenuItem(NSMenu *menu, NSString *title,
                        id target, SEL action, NSString *key) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:(key ?: @"")];
    [item setTarget:target];
    [menu addItem:item];
}

- (void)buildMenus {
    NSMenu *menubar = [[NSMenu alloc] initWithTitle:@""];
    [NSApp setMainMenu:menubar];

    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menubar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"App"];
    [appItem setSubmenu:appMenu];

    NSString *name = [[NSProcessInfo processInfo] processName];
    AddMenuItem(appMenu, [NSString stringWithFormat:@"About %@", name], self, @selector(menuAbout:), nil);
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddMenuItem(appMenu, @"Say Hello", self, @selector(menuSayHello:), @"h");
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddMenuItem(appMenu, [NSString stringWithFormat:@"Quit %@", name], self, @selector(menuQuit:), @"q");

    NSMenuItem *fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [menubar addItem:fileItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileItem setSubmenu:fileMenu];
    AddMenuItem(fileMenu, @"Say Hello", self, @selector(menuSayHello:), @"H");

    NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    [menubar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [viewItem setSubmenu:viewMenu];
    AddMenuItem(viewMenu, @"Enter Full Screen", nil, @selector(toggleFullScreen:), @"f");
}

// ── Axis control helpers ───────────────────────────────────────────────────

- (void)applyDisplaySeconds:(NSInteger)v {
    v = MAX(2, MIN(99, v));
    self.displaySeconds                 = v;
    self.spectrogramView.displaySeconds = v;
    [self.secsField setStringValue:[NSString stringWithFormat:@"%ld", (long)v]];
    [self.spectrogramView setNeedsDisplay:YES];
}

- (void)applyMaxFrequency:(CGFloat)v {
    v = MAX(1000.0, MIN(20000.0, v));
    self.maxFrequency                 = v;
    self.spectrogramView.maxFrequency = v;
    [self.maxHzField setStringValue:[NSString stringWithFormat:@"%.0f", v]];
    [self.spectrogramView setNeedsDisplay:YES];
}

// ── NSTextFieldDelegate ────────────────────────────────────────────────────

- (void)controlTextDidEndEditing:(NSNotification *)obj {
    NSTextField *field = [obj object];
    if      (field == self.secsField)  [self applyDisplaySeconds:[[field stringValue] integerValue]];
    else if (field == self.maxHzField) [self applyMaxFrequency:(CGFloat)[[field stringValue] integerValue]];
}

// ── Widget factory helpers ─────────────────────────────────────────────────

static NSButton *MakeButton(NSView *content, NSString *title,
                             CGFloat x, CGFloat y, CGFloat w, CGFloat h,
                             id target, SEL action) {
    NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(x, y, w, h)];
    [b setTitle:title]; [b setBezelStyle:NSBezelStyleRounded];
    [b setTarget:target]; [b setAction:action];
    [content addSubview:b];
    return b;
}

static void MakeRowLabel(NSView *content, NSString *text,
                         CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
    NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(x, y, w, h)];
    [f setStringValue:text];
    [f setEditable:NO]; [f setBezeled:NO]; [f setDrawsBackground:NO];
    [f setFont:[NSFont systemFontOfSize:12]];
    [f setAlignment:NSTextAlignmentRight];
    [f setAutoresizingMask:NSViewMaxYMargin];
    [content addSubview:f];
}

static NSTextField *MakeInputField(NSView *content, NSString *text,
                                   CGFloat x, CGFloat y, CGFloat w, CGFloat h,
                                   id delegate) {
    NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(x, y, w, h)];
    [f setStringValue:text];
    [f setEditable:YES]; [f setBezeled:YES];
    [f setAlignment:NSTextAlignmentCenter];
    [f setFont:[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular]];
    [f setDelegate:delegate];
    [f setAutoresizingMask:NSViewMaxYMargin];
    [content addSubview:f];
    return f;
}

// ── Main UI assembly ───────────────────────────────────────────────────────

- (void)buildWindowAndUI {
    NSRect frame = NSMakeRect(0, 0, 720, 500);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                       styleMask:(NSWindowStyleMaskTitled      |
                                  NSWindowStyleMaskClosable    |
                                  NSWindowStyleMaskMiniaturizable |
                                  NSWindowStyleMaskResizable)
                         backing:NSBackingStoreBuffered
                           defer:NO];
    [self.window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [self.window setDelegate:self];
    [self.window center];
    [self.window setTitle:@"Realtime Spectrograph"];
    [self.window setMinSize:NSMakeSize(600, 300)];

    NSView *content = [self.window contentView];

    CGFloat bw = 70, bh = 36, gap = 8;
    CGFloat rowY = 8;
    CGFloat x    = 8;
    CGFloat tfH  = 22;
    CGFloat tfY  = rowY + (bh - tfH) * 0.5;

    self.startButton = MakeButton(content, @"Start", x, rowY, bw, bh, self, @selector(playClicked:));
    [self.startButton setAutoresizingMask:NSViewMaxYMargin];
    x += bw + gap;

    self.stopButton = MakeButton(content, @"Stop", x, rowY, bw, bh, self, @selector(stopClicked:));
    [self.stopButton setAutoresizingMask:NSViewMaxYMargin];
    x += bw + gap;

    self.fullscreenButton = MakeButton(content, @"Full", x, rowY, bw, bh,
                                       self, @selector(fullscreenClicked:));
    [self.fullscreenButton setAutoresizingMask:NSViewMaxYMargin];
    x += bw + gap;

    MakeRowLabel(content, @"Secs:", x, tfY, 38, tfH);
    x += 38 + 4;
    self.secsField = MakeInputField(content, @DEFAULT_DISPLAY_SECS_STR, x, tfY, 36, tfH, self);
    x += 36 + gap;

    MakeRowLabel(content, @"Max Hz:", x, tfY, 52, tfH);
    x += 52 + 4;
    self.maxHzField = MakeInputField(content, @DEFAULT_MAX_FREQ_STR, x, tfY, 56, tfH, self);
    x += 56 + gap;

    CGFloat stateW = 80;
    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(x, rowY - 10, stateW, bh)];
    [self.statusLabel setEditable:NO]; [self.statusLabel setBezeled:NO];
    [self.statusLabel setDrawsBackground:NO];
    [self.statusLabel setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightSemibold]];
    [self.statusLabel setAlignment:NSTextAlignmentLeft];
    [self.statusLabel setAutoresizingMask:NSViewMaxYMargin];
    [content addSubview:self.statusLabel];
    x += stateW + gap;

    self.label = [[NSTextField alloc] initWithFrame:
        NSMakeRect(x, rowY - 6, frame.size.width - x - 8, bh - 6)];
    [self.label setEditable:NO]; [self.label setBezeled:NO]; [self.label setDrawsBackground:NO];
    [self.label setFont:[NSFont systemFontOfSize:13]];
    [self.label setStringValue:@""];
    [self.label setAutoresizingMask:NSViewMaxYMargin | NSViewWidthSizable];
    [content addSubview:self.label];

    CGFloat graphY = rowY + bh + gap;
    SpectrogramView *sv = [[SpectrogramView alloc] initWithFrame:
        NSMakeRect(0, graphY, frame.size.width, frame.size.height - graphY)];
    [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content addSubview:sv];
    self.spectrogramView = sv;

    self.displaySeconds = sv.displaySeconds;
    self.maxFrequency   = sv.maxFrequency;
    self.isRunning = NO;
    [self updatePlaybackUI];

    // ── Redraw timer (once per frame) ────────────────────────────────────────────────
    // The spectrogram view has no external data source that pushes updates, so
    // we poll at redraw fps.  The timer runs on the main run loop.
    NSLog(@"[Spectrograph] starting redraw timer");
    [NSTimer scheduledTimerWithTimeInterval:1.0 / (DEFAULT_FPS)
                                    repeats:YES
                                      block:^(NSTimer *t) {
        (void)t;
        [sv setNeedsDisplay:YES];
    }];

    // ── Key monitor ───────────────────────────────────────────────────────
    __weak AppDelegate *weakSelf = self;
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *event) {
        AppDelegate *s = weakSelf;
        if (!s) return event;

        NSEventModifierFlags held = [event modifierFlags] &
            (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
        if (held) return event;

        NSString *ch = [event characters];

        // Space: toggle play/stop globally, even while editing a text field.
        if ([ch isEqualToString:@" "]) {
            if (s.isRunning) [s stop]; else [s play];
            return nil;
        }

        // D: toggle diagnostics
        if ([ch isEqualToString:@"D"]) {
            diagnose = diagnose ^ 1;
            return nil;
        }

        if ([[s.window firstResponder] isKindOfClass:[NSTextView class]])
            return event;

        if ([ch isEqualToString:@"-"]) {
            [s applyDisplaySeconds:s.displaySeconds - 1];
            return nil;
        }
        if ([ch isEqualToString:@"+"] || [ch isEqualToString:@"="]) {
            [s applyDisplaySeconds:s.displaySeconds + 1];
            return nil;
        }
        return event;
    }];

    [self.window setInitialFirstResponder:self.startButton];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.startButton];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSLog(@"[Spectrograph] applicationDidFinishLaunching");
    [self buildMenus];
    [self buildWindowAndUI];
    [NSApp activateIgnoringOtherApps:YES];
    fft_init();        // initialise with defaults; fft_reinit() updates sr after audio starts
    [self play];       // enter running state immediately; buildAudioEngine will auto-start
    [self setupAudio]; // request mic permission → buildAudioEngine on main thread
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

@end

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
