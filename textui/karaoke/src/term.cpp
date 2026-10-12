#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

// --- UTF-8 ----------------------------------------------------------------

static int cp_bytes(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;  // a stray continuation byte counts as one column
}

int utf8_len(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); i += cp_bytes((unsigned char)s[i])) n++;
    return n;
}

size_t utf8_offset(const std::string& s, int n) {
    size_t i = 0;
    while (n-- > 0 && i < s.size()) i += cp_bytes((unsigned char)s[i]);
    return i < s.size() ? i : s.size();
}

// --- the cell buffer ------------------------------------------------------

bool Cell::operator==(const Cell& o) const {
    return st == o.st && strcmp(ch, o.ch) == 0;
}

void Screen::resize(int nw, int nh) {
    w = nw > 1 ? nw : 1;
    h = nh > 1 ? nh : 1;
    cells.assign(w * h, Cell());
    sent.clear();
}

void Screen::clear(Style st) {
    if ((int)cells.size() != w * h) cells.assign(w * h, Cell());
    Cell c; c.st = st;
    for (auto& x : cells) x = c;
}

void Screen::put(int x, int y, const char* cp, int len, Style st) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    Cell& c = cells[y * w + x];
    if (len > 4) len = 4;
    memcpy(c.ch, cp, len);
    c.ch[len] = 0;
    c.st = st;
}

int Screen::text(int x, int y, const std::string& s, Style st) {
    for (size_t i = 0; i < s.size();) {
        int n = cp_bytes((unsigned char)s[i]);
        if (i + n > s.size()) break;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != 0x7f) put(x, y, &s[i], n, st);
        else put(x, y, " ", 1, st);  // tabs and controls become one blank
        x++;
        i += n;
    }
    return x;
}

void Screen::fill(int x, int y, int n, const char* cp, Style st) {
    int len = (int)strlen(cp);
    for (int i = 0; i < n; i++) put(x + i, y, cp, len, st);
}

static void sgr(std::string& out, const Style& st) {
    out += "\x1b[0";
    if (st.attr & A_BOLD) out += ";1";
    if (st.attr & A_DIM) out += ";2";
    if (st.attr & A_UNDERLINE) out += ";4";
    if (st.attr & A_REVERSE) out += ";7";
    char buf[16];
    if (st.fg != C_DEFAULT) {
        snprintf(buf, sizeof buf, ";%d", st.fg < 8 ? 30 + st.fg : 90 + st.fg - 8);
        out += buf;
    }
    if (st.bg != C_DEFAULT) {
        snprintf(buf, sizeof buf, ";%d", st.bg < 8 ? 40 + st.bg : 100 + st.bg - 8);
        out += buf;
    }
    out += "m";
}

std::string Screen::diff(bool full) {
    std::string out;
    if (full || sent.size() != cells.size()) {
        // An empty cell matches nothing, so everything is sent.
        sent.assign(cells.size(), Cell());
        for (auto& c : sent) c.ch[0] = 0;
        out += "\x1b[0m\x1b[2J";
    }
    Style cur; bool have_style = false;
    int cx = -1, cy = -1;
    char buf[32];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const Cell& c = cells[y * w + x];
            if (c == sent[y * w + x]) continue;
            // Never write the bottom-right cell: on many terminals that
            // scrolls the screen by a line.
            if (y == h - 1 && x == w - 1) continue;
            if (cx != x || cy != y) {
                snprintf(buf, sizeof buf, "\x1b[%d;%dH", y + 1, x + 1);
                out += buf;
            }
            if (!have_style || c.st != cur) { sgr(out, c.st); cur = c.st; have_style = true; }
            out += c.ch;
            sent[y * w + x] = c;
            cx = x + 1; cy = y;
        }
    }
    if (have_style) out += "\x1b[0m";
    return out;
}

std::string Screen::plain() const {
    std::string out;
    for (int y = 0; y < h; y++) {
        std::string line;
        for (int x = 0; x < w; x++) line += cells[y * w + x].ch;
        size_t end = line.find_last_not_of(' ');
        line.resize(end == std::string::npos ? 0 : end + 1);
        out += line;
        out += '\n';
    }
    return out;
}

// --- the terminal ---------------------------------------------------------

static int            s_fd = -1;
static bool           s_raw = false;
static struct termios s_saved;
static volatile sig_atomic_t s_winch = 0;

static void on_winch(int) { s_winch = 1; }

static void on_fatal(int sig) {
    term_close();
    signal(sig, SIG_DFL);
    raise(sig);
}

bool term_open() {
    s_fd = open("/dev/tty", O_RDWR);
    if (s_fd < 0) return false;
    if (tcgetattr(s_fd, &s_saved) < 0) return false;
    struct termios t = s_saved;
    // ISIG off as well: ^C, ^Z and friends arrive as keys, and the app
    // decides what they mean.  IEXTEN off frees ^V and ^O.
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(s_fd, TCSAFLUSH, &t) < 0) return false;
    s_raw = true;
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_fatal);
    signal(SIGSEGV, on_fatal);
    signal(SIGABRT, on_fatal);
    term_write("\x1b[?1049h\x1b[?25l\x1b[2J");
    return true;
}

void term_close() {
    if (!s_raw) return;
    term_write("\x1b[0m\x1b[?25h\x1b[?1049l");
    tcsetattr(s_fd, TCSAFLUSH, &s_saved);
    s_raw = false;
}

bool term_size(int* w, int* h) {
    struct winsize ws;
    int fd = s_fd >= 0 ? s_fd : STDOUT_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) return false;
    *w = ws.ws_col;
    *h = ws.ws_row;
    return true;
}

bool term_resized() {
    if (!s_winch) return false;
    s_winch = 0;
    return true;
}

void term_write(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(s_fd, s.data() + off, s.size() - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        off += n;
    }
}

// Decodes what one read() returned.  An ESC with nothing after it in the same
// read is the Escape key; terminals send a sequence in one write, so a lone
// ESC that is really the start of a sequence is rare enough not to matter.
static void decode(const unsigned char* b, int n, std::vector<int>* out) {
    for (int i = 0; i < n;) {
        unsigned char c = b[i];
        if (c != 0x1b) {
            if (c == '\n') c = K_ENTER;
            if (c == 8) c = K_BACKSPACE;
            if (c >= 0x80) {
                // A UTF-8 character typed into a search box: keep the lead
                // byte's code point roughly, as a printable stand-in.
                int len = cp_bytes(c), cp = c & (0xff >> (len + 1));
                for (int k = 1; k < len && i + k < n; k++) cp = (cp << 6) | (b[i + k] & 0x3f);
                out->push_back(cp);
                i += len;
                continue;
            }
            out->push_back(c);
            i++;
            continue;
        }
        if (i + 1 >= n) { out->push_back(K_ESC); i++; continue; }
        unsigned char k = b[i + 1];
        if (k != '[' && k != 'O') { out->push_back(K_ESC); i++; continue; }
        // CSI: parameters, then a final byte in 0x40..0x7e.
        int j = i + 2, param = 0;
        while (j < n && b[j] >= 0x30 && b[j] <= 0x3f) {
            if (b[j] >= '0' && b[j] <= '9') param = param * 10 + (b[j] - '0');
            j++;
        }
        if (j >= n) { i = n; break; }
        unsigned char f = b[j];
        int key = K_NONE;
        switch (f) {
        case 'A': key = K_UP; break;
        case 'B': key = K_DOWN; break;
        case 'C': key = K_RIGHT; break;
        case 'D': key = K_LEFT; break;
        case 'H': key = K_HOME; break;
        case 'F': key = K_END; break;
        case 'Z': key = K_SHIFT_TAB; break;
        case '~':
            switch (param) {
            case 1: case 7: key = K_HOME; break;
            case 4: case 8: key = K_END; break;
            case 3: key = K_DEL; break;
            case 5: key = K_PGUP; break;
            case 6: key = K_PGDN; break;
            }
            break;
        }
        if (key != K_NONE) out->push_back(key);
        i = j + 1;
    }
}

void term_read_keys(int timeout_ms, std::vector<int>* out) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s_fd, &rf);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(s_fd + 1, &rf, nullptr, nullptr, &tv) <= 0) return;
    unsigned char buf[256];
    ssize_t n = read(s_fd, buf, sizeof buf);
    if (n > 0) decode(buf, (int)n, out);
}
