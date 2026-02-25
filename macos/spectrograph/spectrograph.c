#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTextFieldDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) NSTextField *label;         // general status / click feedback
@property (strong) NSTextField *statusLabel;   // Running / Stopped
@property (strong) NSButton *startButton;
@property (strong) NSButton *stopButton;
@property (strong) NSButton *fullscreenButton;
@property (assign) BOOL isRunning;
@end

@implementation AppDelegate

- (void)setStatus:(NSString *)text {
    [self.label setStringValue:text];
}

- (void)updatePlaybackUI {
    if (self.isRunning) {
        [self.statusLabel setStringValue:@"Running"];
        [self.statusLabel setTextColor:[NSColor systemGreenColor]];
        [self.startButton  setEnabled:NO];
        [self.stopButton  setEnabled:YES];
    } else {
        [self.statusLabel setStringValue:@"Stopped"];
        [self.statusLabel setTextColor:[NSColor secondaryLabelColor]];
        [self.startButton  setEnabled:YES];
        [self.stopButton  setEnabled:NO];
    }
}

- (void)play {
    if (!self.isRunning) {
        self.isRunning = YES;
        [self updatePlaybackUI];
        [self setStatus:@"State: Running"];
    }
}

- (void)stop {
    if (self.isRunning) {
        self.isRunning = NO;
        [self updatePlaybackUI];
        [self setStatus:@"State: Stopped"];
    }
}

- (void)playClicked:(id)sender   { (void)sender; [self play]; }
- (void)stopClicked:(id)sender   { (void)sender; [self stop]; }

- (void)toggleFullscreen {
    [self.window toggleFullScreen:nil];
}

- (void)fullscreenClicked:(id)sender {
    (void)sender;
    [self toggleFullscreen];
}

// NSWindowDelegate — keep the button label in sync with fullscreen state.
- (void)windowDidEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    [self.fullscreenButton setTitle:@"Exit Full"];
}

- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;
    [self.fullscreenButton setTitle:@"Full"];
}

- (void)buttonClicked:(id)sender {
    NSString *title = [sender title];
    [self setStatus:[NSString stringWithFormat:@"Clicked: %@", title]];
}

- (void)menuSayHello:(id)sender {
    (void)sender;
    [self setStatus:@"Menu: Hello 👋"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Realtime Audio Spectrograph";
    a.informativeText = @"This alert was triggered from the menu.";
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

- (void)menuAbout:(id)sender {
    (void)sender;
    [self setStatus:@"Menu: About"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"About Spectrograph";
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

    // View menu — nil target lets the responder chain find NSWindow's toggleFullScreen:.
    // macOS automatically flips the title between "Enter Full Screen" and "Exit Full Screen".
    NSMenuItem *viewMenuItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    [menubar addItem:viewMenuItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [viewMenuItem setSubmenu:viewMenu];
    AddMenuItem(viewMenu, @"Enter Full Screen", nil, @selector(toggleFullScreen:), @"f"); // Cmd+F
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
    // Window is 720 pt wide to fit 8 buttons + 3-char tempo display + gaps.
    NSRect frame = NSMakeRect(0, 0, 720, 300);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable |
                                                         NSWindowStyleMaskResizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    // Opt the window into native macOS fullscreen support.
    [self.window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [self.window setDelegate:self];
    [self.window center];
    [self.window setTitle:@"Realtime Spectrograph"];

    NSView *content = [self.window contentView];

    // ── Single bottom row: Buttons | State | Status line ─────────────────
    //   All elements share the same Y and height, pinned to the bottom edge.
    CGFloat bw = 70, bh = 36, gap = 8;
    CGFloat rowY = 8;   // 8 pt margin from the bottom of the window
    CGFloat x = 8;

    // Buttons
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

    // State display — fixed width, right of buttons
    CGFloat stateW = 80;
    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(x, rowY - 10, stateW, bh)];
    [self.statusLabel setEditable:NO];
    [self.statusLabel setBezeled:NO];
    [self.statusLabel setDrawsBackground:NO];
    [self.statusLabel setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightSemibold]];
    [self.statusLabel setAlignment:NSTextAlignmentLeft];
    [self.statusLabel setAutoresizingMask:NSViewMaxYMargin];
    [content addSubview:self.statusLabel];
    x += stateW + gap;

    // Status / event feedback — fills the rest of the row
    self.label = [[NSTextField alloc] initWithFrame:NSMakeRect(x, rowY - 6, 720 - x - 8, bh - 6)];
    [self.label setEditable:NO];
    [self.label setBezeled:NO];
    [self.label setDrawsBackground:NO];
    [self.label setFont:[NSFont systemFontOfSize:13]];
    [self.label setStringValue:@""];
    [self.label setAutoresizingMask:NSViewMaxYMargin | NSViewWidthSizable];
    [content addSubview:self.label];

    // ── Initialise state and sync UI ──────────────────────────────────────
    self.isRunning     = NO;
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
            if (s.isRunning) [s stop]; else [s play];
            return nil;   // consume
        }
        return event;
    }];

    // ── Default focus: Start button, not the text field ────────────────────
    //    setInitialFirstResponder: before makeKeyAndOrderFront: sets the
    //    first-focus target; makeFirstResponder: enforces it immediately.
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

        // Critical: without this, launching from Terminal can behave like an accessory app:
        // no menu bar, and keystrokes keep going to Terminal.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];

        [app run];
    }
    return 0;
}
