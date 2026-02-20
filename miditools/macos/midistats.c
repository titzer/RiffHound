// helloworld.c
// Build:
//   clang -x objective-c -fobjc-arc helloworld.c -framework Cocoa -o HelloWorld
// Run:
//   ./HelloWorld

#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTextFieldDelegate>
@property (strong) NSWindow *window;
@property (strong) NSTextField *label;         // general status / click feedback
@property (strong) NSTextField *statusLabel;   // Playing / Stopped
@property (strong) NSTextField *textField;
@property (strong) NSButton *playButton;
@property (strong) NSButton *stopButton;
@property (strong) NSButton *soundButton;
@property (strong) NSButton *drumsButton;
@property (strong) NSButton *chordsButton;
@property (assign) BOOL isPlaying;
@property (assign) BOOL soundEnabled;
@property (assign) BOOL drumsEnabled;
@property (assign) BOOL chordsEnabled;
@end

@implementation AppDelegate

- (void)setStatus:(NSString *)text {
    [self.label setStringValue:text];
}

- (void)updatePlaybackUI {
    if (self.isPlaying) {
        [self.statusLabel setStringValue:@"Playing"];
        [self.statusLabel setTextColor:[NSColor systemGreenColor]];
        [self.playButton  setEnabled:NO];
        [self.stopButton  setEnabled:YES];
    } else {
        [self.statusLabel setStringValue:@"Stopped"];
        [self.statusLabel setTextColor:[NSColor secondaryLabelColor]];
        [self.playButton  setEnabled:YES];
        [self.stopButton  setEnabled:NO];
    }
}

- (void)play {
    if (!self.isPlaying) {
        self.isPlaying = YES;
        [self updatePlaybackUI];
        [self setStatus:@"State: Playing"];
    }
}

- (void)stop {
    if (self.isPlaying) {
        self.isPlaying = NO;
        [self updatePlaybackUI];
        [self setStatus:@"State: Stopped"];
    }
}

- (void)toggleSound {
    self.soundEnabled = !self.soundEnabled;
    [self.soundButton setState:self.soundEnabled ? NSControlStateValueOn : NSControlStateValueOff];
    [self.soundButton setTitle:self.soundEnabled ? @"Sound" : @"Muted"];
    [self setStatus:[NSString stringWithFormat:@"Sound: %@", self.soundEnabled ? @"On" : @"Off"]];
}

- (void)toggleDrums {
    self.drumsEnabled = !self.drumsEnabled;
    [self.drumsButton setState:self.drumsEnabled ? NSControlStateValueOn : NSControlStateValueOff];
    [self setStatus:[NSString stringWithFormat:@"Drums: %@", self.drumsEnabled ? @"On" : @"Off"]];
}

- (void)toggleChords {
    self.chordsEnabled = !self.chordsEnabled;
    [self.chordsButton setState:self.chordsEnabled ? NSControlStateValueOn : NSControlStateValueOff];
    [self setStatus:[NSString stringWithFormat:@"Chords: %@", self.chordsEnabled ? @"On" : @"Off"]];
}

- (void)playClicked:(id)sender   { (void)sender; [self play]; }
- (void)stopClicked:(id)sender   { (void)sender; [self stop]; }

- (void)soundClicked:(id)sender {
    // NSButtonTypePushOnPushOff already toggled state; read it back, then sync title.
    (void)sender;
    self.soundEnabled = ([self.soundButton state] == NSControlStateValueOn);
    [self.soundButton setTitle:self.soundEnabled ? @"Sound" : @"Muted"];
    [self setStatus:[NSString stringWithFormat:@"Sound: %@", self.soundEnabled ? @"On" : @"Off"]];
}

- (void)drumsClicked:(id)sender {
    (void)sender;
    self.drumsEnabled = ([self.drumsButton state] == NSControlStateValueOn);
    [self setStatus:[NSString stringWithFormat:@"Drums: %@", self.drumsEnabled ? @"On" : @"Off"]];
}

- (void)chordsClicked:(id)sender {
    (void)sender;
    self.chordsEnabled = ([self.chordsButton state] == NSControlStateValueOn);
    [self setStatus:[NSString stringWithFormat:@"Chords: %@", self.chordsEnabled ? @"On" : @"Off"]];
}

- (void)buttonClicked:(id)sender {
    NSString *title = [sender title];
    [self setStatus:[NSString stringWithFormat:@"Clicked: %@", title]];
}

- (void)textFieldAccepted:(id)sender {
    (void)sender;
    NSString *s = [self.textField stringValue];
    if (s.length == 0) s = @"(empty)";
    [self.label setStringValue:s];
    [self.textField setStringValue:@""];
    [self.window makeFirstResponder:self.playButton];
}

- (void)menuSayHello:(id)sender {
    (void)sender;
    [self setStatus:@"Menu: Hello 👋"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"HelloWorld";
    a.informativeText = @"This alert was triggered from the menu.";
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

- (void)menuAbout:(id)sender {
    (void)sender;
    [self setStatus:@"Menu: About"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"About HelloWorld";
    a.informativeText = @"Single-file Cocoa app (built from a .c file compiled as Objective-C).";
    [a addButtonWithTitle:@"Nice"];
    [a runModal];
}

- (void)menuQuit:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}

static void AddMenuItem(NSMenu *menu, NSString *title, id target, SEL action, NSString *keyEquivalent) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:(keyEquivalent ?: @"")];
    [item setTarget:target];
    [menu addItem:item];
}

- (void)buildMenus {
    NSMenu *menubar = [[NSMenu alloc] initWithTitle:@""];
    [NSApp setMainMenu:menubar];

    NSMenuItem *appMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menubar addItem:appMenuItem];

    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"App"];
    [appMenuItem setSubmenu:appMenu];

    NSString *appName = [[NSProcessInfo processInfo] processName];
    AddMenuItem(appMenu, [NSString stringWithFormat:@"About %@", appName], self, @selector(menuAbout:), nil);
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddMenuItem(appMenu, @"Say Hello", self, @selector(menuSayHello:), @"h");
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddMenuItem(appMenu, [NSString stringWithFormat:@"Quit %@", appName], self, @selector(menuQuit:), @"q");

    NSMenuItem *fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [menubar addItem:fileMenuItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenuItem setSubmenu:fileMenu];
    AddMenuItem(fileMenu, @"Say Hello", self, @selector(menuSayHello:), @"H"); // Shift-H
}

static NSButton *MakeButton(NSView *content, NSString *title,
                             CGFloat x, CGFloat y, CGFloat w, CGFloat h,
                             id target, SEL action) {
    NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(x, y, w, h)];
    [b setTitle:title];
    [b setBezelStyle:NSBezelStyleRounded];
    [b setTarget:target];
    [b setAction:action];
    [content addSubview:b];
    return b;
}

static NSButton *MakeToggle(NSView *content, NSString *title,
                              CGFloat x, CGFloat y, CGFloat w, CGFloat h,
                              BOOL initiallyOn, id target, SEL action) {
    NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(x, y, w, h)];
    [b setTitle:title];
    [b setBezelStyle:NSBezelStyleRounded];
    [b setButtonType:NSButtonTypePushOnPushOff];
    [b setState:initiallyOn ? NSControlStateValueOn : NSControlStateValueOff];
    [b setTarget:target];
    [b setAction:action];
    [content addSubview:b];
    return b;
}

- (void)buildWindowAndUI {
    // Window is 640 pt wide to fit 7 buttons + 3-char tempo display + gaps.
    NSRect frame = NSMakeRect(0, 0, 640, 300);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable |
                                                         NSWindowStyleMaskResizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window center];
    [self.window setTitle:@"HelloWorld"];

    NSView *content = [self.window contentView];

    // ── Top row ──────────────────────────────────────────────────────────
    //   Play  Stop  Again  Sound  Tap  [120]  Drums  Chords
    //   7 × 70 pt buttons + 44 pt tempo display + 7 × 8 pt gaps = 590 pt
    //   Centred in 640 pt window → 25 pt margins on each side.
    CGFloat bw = 70, bh = 36, gap = 8;
    CGFloat dw = 44;  // tempo display width (≈ 3 monospaced digits + insets)
    CGFloat sx = 25;
    CGFloat topY = 250;

    // Momentary push buttons — NSViewMinYMargin keeps them pinned to the top
    self.playButton = MakeButton(content, @"Play",  sx+(bw+gap)*0, topY, bw, bh, self, @selector(playClicked:));
    [self.playButton setAutoresizingMask:NSViewMinYMargin];
    self.stopButton = MakeButton(content, @"Stop",  sx+(bw+gap)*1, topY, bw, bh, self, @selector(stopClicked:));
    [self.stopButton setAutoresizingMask:NSViewMinYMargin];
    NSButton *againButton = MakeButton(content, @"Again", sx+(bw+gap)*2, topY, bw, bh, self, @selector(buttonClicked:));
    [againButton setAutoresizingMask:NSViewMinYMargin];
    NSButton *tapButton   = MakeButton(content, @"Tap",   sx+(bw+gap)*4, topY, bw, bh, self, @selector(buttonClicked:));
    [tapButton setAutoresizingMask:NSViewMinYMargin];

    // Sound — toggle, starts ON, title flips between "Sound" and "Muted".
    // Frame is fixed so the button never resizes when the label changes.
    self.soundButton = MakeToggle(content, @"Sound", sx+(bw+gap)*3, topY, bw, bh,
                                  YES, self, @selector(soundClicked:));
    [self.soundButton setAutoresizingMask:NSViewMinYMargin];

    // ── Tempo display: 3-character numeric readout between Tap and Drums ──
    CGFloat tempoX = sx + (bw+gap)*5;   // right of Tap + gap
    NSTextField *tempoDisplay = [[NSTextField alloc] initWithFrame:NSMakeRect(tempoX, topY+4, dw, bh-8)];
    [tempoDisplay setEditable:NO];
    [tempoDisplay setSelectable:NO];
    [tempoDisplay setAlignment:NSTextAlignmentCenter];
    [tempoDisplay setFont:[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular]];
    [tempoDisplay setStringValue:@"120"];
    [tempoDisplay setAutoresizingMask:NSViewMinYMargin];
    [content addSubview:tempoDisplay];

    // Drums — toggle, starts OFF
    CGFloat drumsX = tempoX + dw + gap;
    self.drumsButton  = MakeToggle(content, @"Drums",  drumsX,        topY, bw, bh,
                                   NO,  self, @selector(drumsClicked:));
    [self.drumsButton setAutoresizingMask:NSViewMinYMargin];
    // Chords — toggle, starts ON
    self.chordsButton = MakeToggle(content, @"Chords", drumsX+bw+gap, topY, bw, bh,
                                   YES, self, @selector(chordsClicked:));
    [self.chordsButton setAutoresizingMask:NSViewMinYMargin];

    // ── Status line (middle of window) ───────────────────────────────────
    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 196, 640, 36)];
    [self.statusLabel setEditable:NO];
    [self.statusLabel setBezeled:NO];
    [self.statusLabel setDrawsBackground:NO];
    [self.statusLabel setFont:[NSFont systemFontOfSize:20 weight:NSFontWeightSemibold]];
    [self.statusLabel setAlignment:NSTextAlignmentCenter];
    [content addSubview:self.statusLabel];

    // ── General click / event feedback label ─────────────────────────────
    self.label = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 158, 600, 24)];
    [self.label setEditable:NO];
    [self.label setBezeled:NO];
    [self.label setDrawsBackground:NO];
    [self.label setFont:[NSFont systemFontOfSize:13]];
    [self.label setStringValue:@""];
    [content addSubview:self.label];

    // ── Text field — NSViewMaxYMargin+NSViewWidthSizable pins it to the bottom ──
    self.textField = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 12, 600, 28)];
    [self.textField setPlaceholderString:@"Type here… press Enter to accept"];
    [self.textField setDelegate:self];
    [self.textField setTarget:self];
    [self.textField setAction:@selector(textFieldAccepted:)];
    [self.textField setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:self.textField];

    // ── Initialise state and sync UI ──────────────────────────────────────
    self.isPlaying     = NO;
    self.soundEnabled  = YES;   // Sound on by default
    self.drumsEnabled  = NO;    // Drums off by default
    self.chordsEnabled = YES;   // Chords on by default
    [self updatePlaybackUI];

    // ── Global key monitor (Space / M / D / C) ───────────────────────────
    //    Pass events through when a text field is being edited, and when
    //    any Cmd/Ctrl/Option modifier is held (preserve standard shortcuts).
    __weak AppDelegate *weakSelf = self;
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        AppDelegate *s = weakSelf;
        if (!s) return event;

        // Let the text field handle its own typing.
        if ([[s.window firstResponder] isKindOfClass:[NSTextView class]])
            return event;

        // Ignore when Cmd / Ctrl / Option are held.
        NSEventModifierFlags held = [event modifierFlags] &
            (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
        if (held) return event;

        NSString *ch = [event charactersIgnoringModifiers];
        if ([ch isEqualToString:@" "]) {
            if (s.isPlaying) [s stop]; else [s play];
            return nil;   // consume
        }
        if ([ch isEqualToString:@"m"]) { [s toggleSound];  return nil; }
        if ([ch isEqualToString:@"d"]) { [s toggleDrums];  return nil; }
        if ([ch isEqualToString:@"c"]) { [s toggleChords]; return nil; }
        return event;
    }];

    // ── Default focus: Play button, not the text field ────────────────────
    //    setInitialFirstResponder: before makeKeyAndOrderFront: sets the
    //    first-focus target; makeFirstResponder: enforces it immediately.
    [self.window setInitialFirstResponder:self.playButton];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.playButton];
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

        // Critical: without this, launching from Terminal can behave like an accessory app:
        // no menu bar, and keystrokes keep going to Terminal.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];

        [app run];
    }
    return 0;
}
