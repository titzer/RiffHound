#import <Cocoa/Cocoa.h>

// ════════════════════════════════════════════════════════════════════════════
// SpectrogramView  –  draws axes and grid (spectral data to be added later)
// ════════════════════════════════════════════════════════════════════════════

@interface SpectrogramView : NSView
@property (assign) NSInteger displaySeconds;   // 2–99,        default 10
@property (assign) CGFloat   maxFrequency;     // 1000–20000 Hz, default 8000
@end

@implementation SpectrogramView

static const CGFloat kLeftMargin   = 30.0;    // px reserved for Y-axis labels
static const CGFloat kFreqInterval = 1000.0;  // Hz between horizontal grid lines

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _displaySeconds = 10;
        _maxFrequency   = 8000.0;
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;

    NSRect  b  = self.bounds;
    CGFloat gX = kLeftMargin;             // graph area left edge
    CGFloat gY = 0.0;                     // graph area bottom edge
    CGFloat gW = b.size.width  - gX;     // graph area width
    CGFloat gH = b.size.height;          // graph area height

    // ── 1. Backgrounds ────────────────────────────────────────────────────
    // Dark strip for the Y-axis label margin, pure black for the graph area.
    [[NSColor colorWithWhite:0.06 alpha:1.0] setFill];
    NSRectFill(b);
    [[NSColor blackColor] setFill];
    NSRectFill(NSMakeRect(gX, gY, gW, gH));

    // Shared text attributes
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

    // ── 2. Horizontal grid lines (frequency) + Y-axis labels in margin ────
    CGFloat maxFreq = self.maxFrequency;
    for (CGFloat freq = kFreqInterval; freq < maxFreq; freq += kFreqInterval) {
        CGFloat yPos = gY + (freq / maxFreq) * gH;

        [gridColor setStroke];
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line setLineWidth:0.5];
        [line moveToPoint:NSMakePoint(gX,      yPos)];
        [line lineToPoint:NSMakePoint(gX + gW, yPos)];
        [line stroke];

        // Label sits in the left margin, right-aligned against the graph edge.
        NSString *lbl = (freq >= 1000.0)
            ? [NSString stringWithFormat:@"%.0fk", freq / 1000.0]
            : [NSString stringWithFormat:@"%.0f",  freq];
        NSSize ls = [lbl sizeWithAttributes:yLblAttrs];
        [lbl drawAtPoint:NSMakePoint(gX - ls.width - 4, yPos - ls.height * 0.5)
          withAttributes:yLblAttrs];
    }

    // ── 3. Vertical grid lines (time) + labels drawn inside the graph ─────
    // t=0 is the right edge; negative values extend to the left.
    NSInteger secs = self.displaySeconds;
    for (NSInteger i = 0; i <= secs; i++) {
        CGFloat xPos = gX + gW - (CGFloat)i / (CGFloat)secs * gW;

        [gridColor setStroke];
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line setLineWidth:0.5];
        [line moveToPoint:NSMakePoint(xPos, gY)];
        [line lineToPoint:NSMakePoint(xPos, gY + gH)];
        [line stroke];

        // "0" drawn to the left of its line; all others to the right.
        NSString *lbl = (i == 0)
            ? @"0"
            : [NSString stringWithFormat:@"-%ld", (long)i];
        NSSize   ls  = [lbl sizeWithAttributes:xLblAttrs];
        CGFloat  lx  = (i == 0) ? xPos - ls.width - 2 : xPos + 2;
        [lbl drawAtPoint:NSMakePoint(lx, gY + 4) withAttributes:xLblAttrs];
    }

    // ── 4. Border around the graph area (drawn last, on top) ──────────────
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
    if (!self.isRunning) { self.isRunning = YES;  [self updatePlaybackUI]; [self setStatus:@"State: Running"]; }
}
- (void)stop {
    if (self.isRunning)  { self.isRunning = NO;   [self updatePlaybackUI]; [self setStatus:@"State: Stopped"]; }
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
    AddMenuItem(appMenu, [NSString stringWithFormat:@"About %@", name], self, @selector(menuAbout:),    nil);
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddMenuItem(appMenu, @"Say Hello", self, @selector(menuSayHello:), @"h");
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddMenuItem(appMenu, [NSString stringWithFormat:@"Quit %@", name],  self, @selector(menuQuit:),     @"q");

    NSMenuItem *fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [menubar addItem:fileItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileItem setSubmenu:fileMenu];
    AddMenuItem(fileMenu, @"Say Hello", self, @selector(menuSayHello:), @"H");

    // View menu — nil target lets the responder chain reach NSWindow's toggleFullScreen:.
    NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    [menubar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [viewItem setSubmenu:viewMenu];
    AddMenuItem(viewMenu, @"Enter Full Screen", nil, @selector(toggleFullScreen:), @"f");
}

// ── Axis control helpers ───────────────────────────────────────────────────

- (void)applyDisplaySeconds:(NSInteger)v {
    v = MAX(2, MIN(99, v));
    self.displaySeconds               = v;
    self.spectrogramView.displaySeconds = v;
    [self.secsField setStringValue:[NSString stringWithFormat:@"%ld", (long)v]];
    [self.spectrogramView setNeedsDisplay:YES];
}

- (void)applyMaxFrequency:(CGFloat)v {
    v = MAX(1000.0, MIN(20000.0, v));
    self.maxFrequency                = v;
    self.spectrogramView.maxFrequency = v;
    [self.maxHzField setStringValue:[NSString stringWithFormat:@"%.0f", v]];
    [self.spectrogramView setNeedsDisplay:YES];
}

// ── NSTextFieldDelegate ────────────────────────────────────────────────────

- (void)controlTextDidEndEditing:(NSNotification *)obj {
    NSTextField *field = [obj object];
    if      (field == self.secsField)   [self applyDisplaySeconds:[[field stringValue] integerValue]];
    else if (field == self.maxHzField)  [self applyMaxFrequency:(CGFloat)[[field stringValue] integerValue]];
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

// Non-editable row label pinned to the bottom of the window.
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

// Editable numeric input field pinned to the bottom of the window.
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

    // ── Bottom row layout (left → right) ──────────────────────────────────
    // All controls share rowY and bh; text fields are centered vertically.
    CGFloat bw = 70, bh = 36, gap = 8;
    CGFloat rowY = 8;
    CGFloat x    = 8;
    CGFloat tfH  = 22;
    CGFloat tfY  = rowY + (bh - tfH) * 0.5;   // center text fields in the row

    // Playback buttons
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

    // ── Secs control ──────────────────────────────────────────────────────
    MakeRowLabel(content, @"Secs:", x, tfY, 38, tfH);
    x += 38 + 4;
    self.secsField = MakeInputField(content, @"10", x, tfY, 36, tfH, self);
    x += 36 + gap;

    // ── Max Hz control ────────────────────────────────────────────────────
    MakeRowLabel(content, @"Max Hz:", x, tfY, 52, tfH);
    x += 52 + 4;
    self.maxHzField = MakeInputField(content, @"8000", x, tfY, 56, tfH, self);
    x += 56 + gap;

    // ── Running / Stopped label ───────────────────────────────────────────
    CGFloat stateW = 80;
    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(x, rowY - 10, stateW, bh)];
    [self.statusLabel setEditable:NO]; [self.statusLabel setBezeled:NO];
    [self.statusLabel setDrawsBackground:NO];
    [self.statusLabel setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightSemibold]];
    [self.statusLabel setAlignment:NSTextAlignmentLeft];
    [self.statusLabel setAutoresizingMask:NSViewMaxYMargin];
    [content addSubview:self.statusLabel];
    x += stateW + gap;

    // ── Feedback label (stretches to fill remaining width) ────────────────
    self.label = [[NSTextField alloc] initWithFrame:
        NSMakeRect(x, rowY - 6, frame.size.width - x - 8, bh - 6)];
    [self.label setEditable:NO]; [self.label setBezeled:NO]; [self.label setDrawsBackground:NO];
    [self.label setFont:[NSFont systemFontOfSize:13]];
    [self.label setStringValue:@""];
    [self.label setAutoresizingMask:NSViewMaxYMargin | NSViewWidthSizable];
    [content addSubview:self.label];

    // ── Spectrogram view (fills everything above the bottom row) ──────────
    CGFloat graphY = rowY + bh + gap;   // 52 pt from the bottom
    SpectrogramView *sv = [[SpectrogramView alloc] initWithFrame:
        NSMakeRect(0, graphY, frame.size.width, frame.size.height - graphY)];
    // Width and height both flexible so the graph fills the window in fullscreen.
    [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content addSubview:sv];
    self.spectrogramView = sv;

    // ── Initialise axis state from view defaults ───────────────────────────
    self.displaySeconds = sv.displaySeconds;   // 10
    self.maxFrequency   = sv.maxFrequency;     // 8000

    self.isRunning = NO;
    [self updatePlaybackUI];

    // ── Key monitor ───────────────────────────────────────────────────────
    // Space  → toggle play/stop
    // -      → decrease display seconds
    // + or = → increase display seconds
    // Events are passed through when a text field is being edited.
    __weak AppDelegate *weakSelf = self;
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *event) {
        AppDelegate *s = weakSelf;
        if (!s) return event;

        // Preserve Cmd / Ctrl / Option shortcuts.
        NSEventModifierFlags held = [event modifierFlags] &
            (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
        if (held) return event;

        NSString *ch = [event characters];

        // Space toggles play/stop globally, even while editing a text field.
        if ([ch isEqualToString:@" "]) {
            if (s.isRunning) [s stop]; else [s play];
            return nil;
        }

        // All other hotkeys pass through when a text field is being edited.
        if ([[s.window firstResponder] isKindOfClass:[NSTextView class]])
            return event;
        if ([ch isEqualToString:@"-"]) {
            [s applyDisplaySeconds:s.displaySeconds - 1];
            return nil;
        }
        // "+" requires Shift on US keyboards; also accept bare "=" as a convenience.
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
    [self buildMenus];
    [self buildWindowAndUI];
    [NSApp activateIgnoringOtherApps:YES];
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
