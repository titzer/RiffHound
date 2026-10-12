#pragma once
// Terminal layer: a cell buffer that is diffed against what was last sent, so
// a frame costs the bytes that changed rather than the whole screen; raw-mode
// keyboard input with the few escape sequences we care about decoded.
//
// Output goes to /dev/tty, not stdout.  The audio code prints its progress to
// stdout and stderr, and main() points both at a log file, so the screen is
// never scribbled on by a printf it did not ask for.

#include <stdint.h>
#include <string>
#include <vector>

// Colours: the 16 ANSI ones, plus DEFAULT for "whatever the terminal uses".
enum Color : uint8_t {
    C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_WHITE,
    C_GRAY, C_BRED, C_BGREEN, C_BYELLOW, C_BBLUE, C_BMAGENTA, C_BCYAN, C_BWHITE,
    C_DEFAULT = 255,
};

enum : uint8_t { A_BOLD = 1, A_DIM = 2, A_REVERSE = 4, A_UNDERLINE = 8 };

struct Style {
    uint8_t fg = C_DEFAULT, bg = C_DEFAULT, attr = 0;
    bool operator==(const Style& o) const { return fg == o.fg && bg == o.bg && attr == o.attr; }
    bool operator!=(const Style& o) const { return !(*this == o); }
};

inline Style S(uint8_t fg, uint8_t attr = 0, uint8_t bg = C_DEFAULT) {
    Style s; s.fg = fg; s.attr = attr; s.bg = bg; return s;
}

// One cell holds one code point of display width 1, as UTF-8.  Everything we
// draw is box drawing, blocks or Latin text, so width 1 is the rule; a wide
// character in a lyric costs one misaligned column, not a broken screen.
struct Cell {
    char  ch[5] = {' ', 0, 0, 0, 0};
    Style st;
    bool operator==(const Cell& o) const;
};

class Screen {
public:
    int w = 80, h = 24;

    void resize(int w, int h);
    void clear(Style st = Style());
    void put(int x, int y, const char* utf8cp, int len, Style st);
    // Draws a UTF-8 string from (x, y), clipped to the screen; returns the
    // column after the last code point.
    int  text(int x, int y, const std::string& s, Style st);
    void fill(int x, int y, int n, const char* utf8cp, Style st);

    // Escape-sequence diff against the previous frame.  full forces a repaint.
    std::string diff(bool full);
    // The frame as plain text, one line per row, trailing blanks trimmed.
    std::string plain() const;

private:
    std::vector<Cell> cells, sent;
};

// Code points in a UTF-8 string, and the byte offset of the nth one.
int    utf8_len(const std::string& s);
size_t utf8_offset(const std::string& s, int n);

// --- the real terminal ---------------------------------------------------

enum Key {
    K_NONE = 0,
    K_UP = 0x10000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_DEL, K_SHIFT_TAB, K_ESC,
    K_ENTER = 13, K_TAB = 9, K_BACKSPACE = 127,
};
inline int ctrl(char c) { return c & 0x1f; }

bool term_open();                 // raw mode, alternate screen, hidden cursor
void term_close();                // restores everything; safe to call twice
bool term_size(int* w, int* h);
bool term_resized();              // SIGWINCH since the last call
void term_write(const std::string& s);
// Waits up to timeout_ms for input and appends decoded keys to out.
void term_read_keys(int timeout_ms, std::vector<int>* out);
