// Everything drawn in the window: toolbar, readout, cents meter, pitch
// history, piano, and the settings window.  Layout scales with the window,
// so fullscreen is the same picture made big.

#include "tuner.h"
#include "audio_io.h"
#include "platform.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Palette (shared with webui/voice)

#define COL_BG      IM_COL32(0x10, 0x12, 0x16, 255)
#define COL_PANEL   IM_COL32(0x0f, 0x13, 0x19, 255)
#define COL_LINE    IM_COL32(0x30, 0x38, 0x46, 255)
#define COL_GRID    IM_COL32(0x25, 0x2e, 0x3d, 255)
#define COL_TEXT    IM_COL32(0xee, 0xf2, 0xf7, 255)
#define COL_MUTED   IM_COL32(0x9a, 0xa7, 0xb7, 255)
#define COL_DIM     IM_COL32(0x4a, 0x55, 0x66, 255)
#define COL_ACCENT  IM_COL32(0x7d, 0xd3, 0xfc, 255)
#define COL_GOOD    IM_COL32(0x86, 0xef, 0xac, 255)
#define COL_WARN    IM_COL32(0xfd, 0xe6, 0x8a, 255)
#define COL_BAD     IM_COL32(0xfc, 0xa5, 0xa5, 255)

static ImU32 with_alpha(ImU32 c, float a) {
    int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}

// ---------------------------------------------------------------------------
// Fonts.  ImGui 1.91 rasterizes fonts up front, so a ladder of sizes is
// loaded and each draw picks the nearest one at or above the size wanted.

static const float TEXT_SIZES[] = { 10, 12, 14, 17, 21, 26, 32, 40, 52 };
static const int   N_TEXT = sizeof(TEXT_SIZES) / sizeof(TEXT_SIZES[0]);
static const float BIG_SIZES[] = { 110, 200 };   // readout glyphs only
static const int   N_BIG = 2;
static ImFont* s_text[N_TEXT];
static ImFont* s_big[N_BIG];

void ui_load_fonts(float density) {
    ImGuiIO& io = ImGui::GetIO();
    static const char* FACES[] = {
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    const char* face = nullptr;
    for (const char* f : FACES) if (access(f, R_OK) == 0) { face = f; break; }

    // ASCII plus the cent and plus-minus signs: all the labels need.
    static const ImWchar LATIN[] = { 0x0020, 0x007E, 0x00A2, 0x00A2, 0x00B1, 0x00B1, 0 };
    static ImVector<ImWchar> big_ranges;
    {
        ImFontGlyphRangesBuilder b;
        b.AddText("ABCDEFG#0123456789+-. Hz");
        b.AddChar(0x00A2);   // cent sign
        b.BuildRanges(&big_ranges);
    }

    io.Fonts->AddFontDefault();   // first font = ImGui UI font, as in beatmapper
    auto load = [&](float px, const ImWchar* ranges, float dens) -> ImFont* {
        ImFontConfig cfg;
        cfg.SizePixels        = px;
        cfg.RasterizerDensity = dens;
        cfg.OversampleH       = 1;
        if (face) {
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(face, px, &cfg, ranges)) return f;
        }
        return io.Fonts->AddFontDefault(&cfg);
    };
    for (int i = 0; i < N_TEXT; i++) s_text[i] = load(TEXT_SIZES[i], LATIN, density);
    // The big faces are drawn scaled down most of the time; density 1 keeps
    // the atlas small and still looks sharp at fullscreen sizes.
    for (int i = 0; i < N_BIG; i++) s_big[i] = load(BIG_SIZES[i], big_ranges.Data, density > 1.5f ? 1.5f : density);
}

static ImFont* font_for(float px) {
    for (int i = 0; i < N_TEXT; i++) if (TEXT_SIZES[i] >= px * 0.95f) return s_text[i];
    for (int i = 0; i < N_BIG; i++)  if (BIG_SIZES[i] >= px * 0.95f) return s_big[i];
    return s_big[N_BIG - 1];
}
static ImVec2 text_size(float px, const char* s) {
    return font_for(px)->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
}
static void text_at(ImDrawList* dl, float px, ImVec2 p, ImU32 c, const char* s) {
    dl->AddText(font_for(px), px, p, c, s);
}
static void text_centered(ImDrawList* dl, float px, float cx, float y, ImU32 c, const char* s) {
    ImVec2 sz = text_size(px, s);
    text_at(dl, px, ImVec2(floorf(cx - sz.x * 0.5f), y), c, s);
}

void ui_apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding    = 0.0f;
    st.FrameRounding     = 3.0f;
    st.ScrollbarRounding = 3.0f;
    st.GrabRounding      = 3.0f;
    st.Colors[ImGuiCol_WindowBg]  = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
    st.Colors[ImGuiCol_PopupBg]   = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
    st.Colors[ImGuiCol_Border]    = ImVec4(0.19f, 0.22f, 0.27f, 1.0f);
}

// ---------------------------------------------------------------------------
// Geometry helpers

static ImU32 cents_color(const Settings& s, float c) {
    float a = fabsf(c);
    return a <= s.in_tune_cents ? COL_GOOD : a <= s.close_cents ? COL_WARN : COL_BAD;
}

static int piano_min(const Settings& s) { return 36 + 12 * s.octave_shift; }   // C2 at shift 0
static const int PIANO_N = 37;                                                // C to C, three octaves

struct PianoGeom {
    float x, y, w, h;
    float u;        // chromatic slot width (top section, history columns)
    float wkW;      // white key width (bottom section)
    float bkH, bkW;
    int   minM, maxM, numWhite;
    int   wkIdx[PIANO_N];

    void init(float x_, float y_, float w_, float h_, int min_m) {
        x = x_; y = y_; w = w_; h = h_;
        minM = min_m; maxM = min_m + PIANO_N - 1;
        numWhite = 0;
        for (int m = minM; m <= maxM; m++) wkIdx[m - minM] = midi_is_black(m) ? -1 : numWhite++;
        u   = w / PIANO_N;
        wkW = u * 12.0f / 7.0f;
        bkH = roundf(h * 0.62f);
        bkW = roundf(u * 0.65f);
    }
    // Bottom-section bounds (relative).  Seven white keys span exactly one
    // chromatic octave, so B|C lines fall on chromatic slots; E|F is pinned
    // to F's slot so the top and bottom halves share the line.
    float wl(int m) const {
        if (((m % 12) + 12) % 12 == 5) return roundf((m - minM) * u);
        return roundf(wkIdx[m - minM] * wkW);
    }
    float wr(int m) const {
        if (((m % 12) + 12) % 12 == 4) return roundf((m + 1 - minM) * u);
        if (wkIdx[m - minM] + 1 >= numWhite) return w;
        return roundf((wkIdx[m - minM] + 1) * wkW);
    }
    float slot_cx(float midi) const { return x + (midi - minM + 0.5f) * u; }

    int hit(float px, float py) const {
        px -= x; py -= y;
        if (px < 0 || px >= w || py < 0 || py >= h) return -1;
        if (py < bkH) {
            for (int m = minM; m <= maxM; m++) {
                if (!midi_is_black(m)) continue;
                float kx = roundf((m - minM + 0.5f) * u - bkW * 0.5f);
                if (px >= kx && px < kx + bkW) return m;
            }
            int slot = (int)floorf(px / u);
            if (slot >= 0 && slot < PIANO_N && !midi_is_black(minM + slot)) return minM + slot;
            return -1;
        }
        for (int m = minM; m <= maxM; m++)
            if (!midi_is_black(m) && px >= wl(m) && px < wr(m)) return m;
        return -1;
    }
};

// Computer-keyboard layout, indexed by semitone offset from the lowest key.
// Z row = first octave (black keys on the A row), Q row = next one and a
// half (black keys on the number row) -- the usual tracker layout.
static const ImGuiKey PIANO_KEYS[PIANO_N] = {
    ImGuiKey_Z, ImGuiKey_S, ImGuiKey_X, ImGuiKey_D, ImGuiKey_C, ImGuiKey_V, ImGuiKey_G,
    ImGuiKey_B, ImGuiKey_H, ImGuiKey_N, ImGuiKey_J, ImGuiKey_M, ImGuiKey_Comma, ImGuiKey_L,
    ImGuiKey_Period, ImGuiKey_Semicolon, ImGuiKey_Slash,
    ImGuiKey_Q, ImGuiKey_2, ImGuiKey_W, ImGuiKey_3, ImGuiKey_E, ImGuiKey_4, ImGuiKey_R,
    ImGuiKey_T, ImGuiKey_6, ImGuiKey_Y, ImGuiKey_7, ImGuiKey_U, ImGuiKey_I, ImGuiKey_9,
    ImGuiKey_O, ImGuiKey_0, ImGuiKey_P, ImGuiKey_Minus, ImGuiKey_LeftBracket, ImGuiKey_RightBracket,
};
static const char* const PIANO_KEY_LABELS[PIANO_N] = {
    "Z", "S", "X", "D", "C", "V", "G", "B", "H", "N", "J", "M", ",", "L", ".", ";", "/",
    "Q", "2", "W", "3", "E", "4", "R", "T", "6", "Y", "7", "U", "I", "9", "O", "0", "P", "-", "[", "]",
};
static int s_key_note[PIANO_N] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static void held_remove(Tuner& t, int m) {
    for (size_t i = t.held.size(); i-- > 0; )
        if (t.held[i] == m) { t.held.erase(t.held.begin() + i); return; }
}

void ui_set_tone(Tuner& t) {
    int note = t.held.empty() ? -1 : t.held.back();
    t.tone_note = note;
    tone_set(note >= 0 ? midi_to_freq((float)note, t.s.a4) : 0.0f, t.s.tone_wave, t.s.tone_volume);
}

// What the readout, meter and piano show: the gated active note and the
// pitch relative to it, or the last such reading (dimmed) when there is none.
struct Reading {
    int   note  = -1;      // -1 = nothing to show
    float cents = 0.0f;
    float freq  = 0.0f;
    bool  live  = false;
};
static Reading reading(const Tuner& t) {
    Reading r;
    if (t.note >= 0 && t.freq > 0) {
        r.note = t.note; r.freq = t.freq; r.live = true;
        r.cents = (freq_to_midi(t.freq, t.s.a4) - t.note) * 100.0f;
    } else if (t.last_note >= 0) {
        r.note = t.last_note; r.freq = t.last_freq; r.cents = t.last_cents;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Readout: note name, cents, frequency

static void draw_readout(Tuner& t, ImDrawList* dl, float x, float y, float w, float h, float sc) {
    Reading rd = reading(t);
    bool  live = rd.live;
    int   note = rd.note;
    float cents = rd.cents;

    float px = h * 1.12f;                    // note-name size; caps fill ~80% of the box
    float oct_px = px * 0.40f, c_px = px * 0.40f, hz_px = px * 0.21f;
    // Fixed slot widths so nothing shifts as the note and digits change.
    auto slots = [&](float p, float* nw, float* ow, float* cw) {
        *nw = text_size(p, "G#").x;
        *ow = text_size(p * 0.40f, "8").x;
        *cw = std::max(text_size(p * 0.40f, "+00\xC2\xA2").x, text_size(p * 0.21f, "0000.0 Hz").x);
    };
    float nw, ow, cw;
    slots(px, &nw, &ow, &cw);
    float gap = px * 0.18f;
    float total = nw + ow + gap + cw;
    if (total > w * 0.96f) {
        px *= w * 0.96f / total;
        oct_px = px * 0.40f; c_px = px * 0.40f; hz_px = px * 0.21f;
        slots(px, &nw, &ow, &cw);
        gap = px * 0.18f;
        total = nw + ow + gap + cw;
    }
    float x0 = floorf(x + (w - total) * 0.5f);
    // Helvetica caps sit between ~0.05 and ~0.77 of the font size below the
    // line top; centre that band in the box.
    float cap_top = 0.05f, cap_bot = 0.77f;
    float top = y + h * 0.5f - (cap_top + cap_bot) * 0.5f * px;
    float base = top + cap_bot * px;         // baseline of the note name

    ImU32 note_col = !live ? COL_DIM : COL_TEXT;
    ImU32 c_col    = !live ? COL_DIM : cents_color(t.s, cents);
    char buf[64];

    if (note >= 0) {
        const char* name = pitch_class_name(note);
        float tw = text_size(px, name).x;
        text_at(dl, px, ImVec2(x0 + nw - tw, top), note_col, name);
        snprintf(buf, sizeof(buf), "%d", midi_octave(note));
        text_at(dl, oct_px, ImVec2(x0 + nw + px * 0.02f, base - cap_bot * oct_px), with_alpha(note_col, 0.8f), buf);
    } else {
        float tw = text_size(px, "-").x;
        text_at(dl, px, ImVec2(x0 + nw - tw, top), COL_DIM, "-");
    }

    float cx = x0 + nw + ow + gap;
    // Cents on the cap line, Hz under it, both inside the note's cap band.
    if (note >= 0) snprintf(buf, sizeof(buf), "%+d\xC2\xA2", (int)lroundf(cents));
    else           snprintf(buf, sizeof(buf), "--\xC2\xA2");
    text_at(dl, c_px, ImVec2(cx, top + cap_top * (px - c_px)), c_col, buf);

    float f = rd.freq;
    if (f > 0) snprintf(buf, sizeof(buf), "%.1f Hz", f);
    else       snprintf(buf, sizeof(buf), "-- Hz");
    float hz_y = base - cap_bot * hz_px - px * 0.02f;
    text_at(dl, hz_px, ImVec2(cx, hz_y), live ? COL_MUTED : COL_DIM, buf);
    if (note >= 0 && hz_px * 0.75f >= 9.0f) {
        snprintf(buf, sizeof(buf), "%.1f", midi_to_freq((float)note, t.s.a4));
        float tp = hz_px * 0.75f;
        float ty = hz_y - tp * 1.15f;
        if (ty > top + c_px * 0.85f)
            text_at(dl, tp, ImVec2(cx, ty), COL_DIM, buf);
    }
    (void)sc;
}

// ---------------------------------------------------------------------------
// Cents meter

static void draw_meter(Tuner& t, ImDrawList* dl, float x, float y, float w, float h, float sc) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), COL_PANEL, 4.0f * sc);
    float pad = 8.0f * sc;
    float x0 = x + pad, x1 = x + w - pad;
    auto cx = [&](float c) { return x0 + (c + 50.0f) / 100.0f * (x1 - x0); };

    // Tolerance zones.
    dl->AddRectFilled(ImVec2(cx(-t.s.close_cents), y + 1), ImVec2(cx(t.s.close_cents), y + h - 1),
                      IM_COL32(0xfd, 0xe6, 0x8a, 14));
    dl->AddRectFilled(ImVec2(cx(-t.s.in_tune_cents), y + 1), ImVec2(cx(t.s.in_tune_cents), y + h - 1),
                      IM_COL32(0x86, 0xef, 0xac, 38));

    bool labels = h >= 30.0f;
    float lab_px = std::min(std::max(h * 0.22f, 10.0f), 26.0f);
    float tick_h = labels ? h - lab_px - 6.0f * sc : h;
    float mid = y + (labels ? 3.0f * sc + tick_h * 0.5f : h * 0.5f);
    float lw = std::max(1.0f, sc);
    for (int c = -50; c <= 50; c += 5) {
        float len = c == 0 ? 0.8f : (c % 25 == 0) ? 0.55f : (c % 10 == 0) ? 0.35f : 0.18f;
        if (c % 10 != 0 && c % 25 != 0 && w < 400) continue;
        ImU32 col = c == 0 ? COL_MUTED : COL_LINE;
        float X = roundf(cx((float)c));
        dl->AddLine(ImVec2(X, mid - tick_h * len * 0.5f), ImVec2(X, mid + tick_h * len * 0.5f), col, c == 0 ? lw * 1.5f : lw);
    }
    if (labels) {
        static const int L[] = { -50, -25, 0, 25, 50 };
        for (int c : L) {
            char b[8];
            snprintf(b, sizeof(b), c > 0 ? "+%d" : "%d", c);
            text_centered(dl, lab_px, cx((float)c), y + h - lab_px - 2.0f * sc, COL_DIM, b);
        }
    }

    Reading rd = reading(t);
    bool live = rd.live;
    if (rd.note >= 0) {
        float c = rd.cents;
        float cl = std::max(-50.0f, std::min(50.0f, c));
        float X = cx(cl);
        ImU32 col = live ? cents_color(t.s, c) : COL_DIM;
        float th = std::min(tick_h * 0.45f, 26.0f * sc);
        dl->AddLine(ImVec2(X, y + 2), ImVec2(X, y + (labels ? 3.0f * sc + tick_h : h) - 2), col, 2.0f * sc);
        dl->AddTriangleFilled(ImVec2(X, y + 2 + th), ImVec2(X - th * 0.5f, y + 2), ImVec2(X + th * 0.5f, y + 2), col);
    }
}

// ---------------------------------------------------------------------------
// Pitch history: time runs upward, columns line up with the piano's
// chromatic slots below it.

static void draw_history(Tuner& t, ImDrawList* dl, const PianoGeom& g, float y, float h, double now, float sc) {
    float x = g.x, w = g.w;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), COL_PANEL);
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);

    for (int m = g.minM; m <= g.maxM; m++) {
        float xl = x + roundf((m - g.minM) * g.u), xr = x + roundf((m - g.minM + 1) * g.u);
        if (midi_is_black(m)) {
            dl->AddRectFilled(ImVec2(xl, y), ImVec2(xr, y + h), IM_COL32(0, 0, 0, 56));
        } else {
            ImU32 c = (m % 12 == 0) ? COL_LINE : COL_GRID;
            dl->AddLine(ImVec2(xl + 0.5f, y), ImVec2(xl + 0.5f, y + h), c, 1.0f);
        }
    }

    double span = t.s.history_s;
    auto ty = [&](double tt) { return (float)(y + h - (now - tt) / span * h); };
    float lo = g.minM - 0.5f, hi = g.maxM + 0.5f;
    auto in_range = [&](float m) { return m >= lo && m <= hi; };
    auto fx = [&](float m) { return g.slot_cx(m); };
    float row = std::max(1.0f, sc);

    // Reference-tone boxes, then note bands and error bars.
    for (const HistPoint& p : t.history) {
        if (now - p.t > span) continue;
        float Y = ty(p.t);
        if (p.ref_note >= 0 && in_range((float)p.ref_note))
            dl->AddRectFilled(ImVec2(fx(p.ref_note - 0.5f), Y - row), ImVec2(fx(p.ref_note + 0.5f), Y + row),
                              IM_COL32(74, 222, 128, 70));
        if (p.midi <= 0 || !in_range(p.midi)) continue;
        float n = roundf(p.midi);
        if (fabsf(p.midi - n) < 0.3f)
            dl->AddRectFilled(ImVec2(fx(n - 0.5f), Y - row), ImVec2(fx(n + 0.5f), Y + 2 * row),
                              IM_COL32(125, 211, 252, 24));
        float e = p.err / 100.0f;
        dl->AddLine(ImVec2(fx(p.midi - e), Y), ImVec2(fx(p.midi + e), Y), IM_COL32(125, 211, 252, 60), row);
    }

    // Pitch line, broken at gaps and out-of-range stretches.
    ImVector<ImVec2> pts;
    auto flush = [&]() {
        if (pts.Size >= 2) dl->AddPolyline(pts.Data, pts.Size, COL_ACCENT, ImDrawFlags_None, 2.0f * sc);
        pts.resize(0);
    };
    for (const HistPoint& p : t.history) {
        if (now - p.t > span || p.midi <= 0 || !in_range(p.midi)) { flush(); continue; }
        pts.push_back(ImVec2(fx(p.midi), ty(p.t)));
    }
    flush();

    // Pitch off the keyboard: an arrow at the edge it went off.
    float midi = t.freq > 0 ? freq_to_midi(t.freq, t.s.a4) : 0.0f;
    if (midi > 0 && !in_range(midi)) {
        float s = 7.0f * sc, yy = y + h - 12.0f * sc;
        if (midi < lo) dl->AddTriangleFilled(ImVec2(x + 4 * sc, yy), ImVec2(x + 4 * sc + s, yy - s), ImVec2(x + 4 * sc + s, yy + s), COL_WARN);
        else           dl->AddTriangleFilled(ImVec2(x + w - 4 * sc, yy), ImVec2(x + w - 4 * sc - s, yy - s), ImVec2(x + w - 4 * sc - s, yy + s), COL_WARN);
    }
    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// Piano

static void draw_piano(Tuner& t, ImDrawList* dl, const PianoGeom& g) {
    int voice = t.freq > 0 ? t.note : -1;
    int pressed = t.tone_note;
    float X = g.x, Y = g.y, H = g.h;

    auto white_col = [&](int m) {
        bool p = m == pressed, v = m == voice;
        return p && v ? IM_COL32(0x38, 0xd0, 0x80, 255) : p ? IM_COL32(0x4c, 0xaf, 0x7d, 255)
             : v ? IM_COL32(0x1a, 0x8f, 0xb5, 255) : IM_COL32(0xc8, 0xd4, 0xe0, 255);
    };
    dl->AddRectFilled(ImVec2(X, Y), ImVec2(X + g.w, Y + H), COL_PANEL);
    for (int m = g.minM; m <= g.maxM; m++) {
        if (midi_is_black(m)) continue;
        int s = m - g.minM;
        dl->AddRectFilled(ImVec2(X + g.wl(m), Y + g.bkH), ImVec2(X + g.wr(m), Y + H), white_col(m));
        dl->AddRectFilled(ImVec2(X + roundf(s * g.u), Y), ImVec2(X + roundf((s + 1) * g.u), Y + g.bkH), white_col(m));
    }
    for (int m = g.minM + 1; m <= g.maxM; m++) {
        if (midi_is_black(m)) continue;
        dl->AddRectFilled(ImVec2(X + g.wl(m), Y + g.bkH), ImVec2(X + g.wl(m) + 1, Y + H), COL_PANEL);
        if (!midi_is_black(m - 1)) {
            float lx = X + roundf((m - g.minM) * g.u);
            dl->AddRectFilled(ImVec2(lx, Y), ImVec2(lx + 1, Y + g.bkH), COL_PANEL);
        }
    }
    for (int m = g.minM; m <= g.maxM; m++) {
        if (!midi_is_black(m)) continue;
        bool p = m == pressed, v = m == voice;
        ImU32 c = p && v ? IM_COL32(0x1a, 0x70, 0x50, 255) : p ? IM_COL32(0x1a, 0x5a, 0x35, 255)
                : v ? IM_COL32(0x10, 0x60, 0xa0, 255) : IM_COL32(0x1c, 0x24, 0x35, 255);
        float kx = X + roundf((m - g.minM + 0.5f) * g.u - g.bkW * 0.5f);
        dl->AddRectFilled(ImVec2(kx, Y), ImVec2(kx + g.bkW, Y + g.bkH), c);
    }

    // Labels: note names on white keys (and on a lit black key), computer
    // keys above them.
    float note_px = std::max(8.0f, std::min(g.wkW * 0.42f, (H - g.bkH) * 0.42f));
    float key_px  = std::max(8.0f, std::min(g.u * 0.62f, note_px));
    for (int m = g.minM; m <= g.maxM; m++) {
        bool black = midi_is_black(m), p = m == pressed, v = m == voice;
        if (black && !p && !v) continue;
        char name[8];
        snprintf(name, sizeof(name), "%s%d", pitch_class_name(m), midi_octave(m));
        float cx = black ? g.slot_cx((float)m) : X + (g.wl(m) + g.wr(m)) * 0.5f;
        ImU32 c = p && v ? IM_COL32(255, 255, 255, 255) : p ? IM_COL32(0x1f, 0x4d, 0x33, 255)
                : v ? IM_COL32(255, 255, 255, 255) : (m % 12 == 0 ? IM_COL32(0x5a, 0x6f, 0x88, 255) : IM_COL32(0x84, 0x94, 0xa6, 255));
        if (black) { if (g.bkW < key_px * 1.6f) continue; }
        float ny = black ? Y + g.bkH + 2 : Y + H - note_px - 3;
        if (!black && g.wkW < text_size(note_px, name).x + 2) {
            if (m % 12 != 0 && !p && !v) continue;   // tight: name only the Cs
        }
        text_centered(dl, note_px, cx, ny, c, name);
    }
    if (t.s.show_key_labels) {
        for (int i = 0; i < PIANO_N; i++) {
            int m = g.minM + i;
            bool p = m == pressed;
            if (!midi_is_black(m)) {
                float cx = X + (g.wl(m) + g.wr(m)) * 0.5f;
                float ky = Y + H - note_px - key_px - 6;
                if (ky < Y + g.bkH + 1) continue;
                text_centered(dl, key_px, cx, ky, p ? IM_COL32(0x1f, 0x4d, 0x33, 255) : IM_COL32(0x3b, 0x4b, 0x5c, 255), PIANO_KEY_LABELS[i]);
            } else {
                if (g.bkW < key_px) continue;
                text_centered(dl, key_px, g.slot_cx((float)m), Y + g.bkH - key_px - 4,
                              p ? IM_COL32(0xc9, 0xf7, 0xdb, 255) : IM_COL32(0x84, 0x96, 0xab, 255), PIANO_KEY_LABELS[i]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Toolbar (hidden in fullscreen)

static void level_bar(Tuner& t, float w, float h, bool show_gate) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), COL_PANEL, 2.0f);
    auto fx = [&](float db) { return p.x + w * std::max(0.0f, std::min(1.0f, (db + 90.0f) / 90.0f)); };
    if (t.mic_on) {
        ImU32 c = t.level_db >= t.s.gate_db ? COL_GOOD : COL_DIM;
        dl->AddRectFilled(ImVec2(p.x + 1, p.y + 2), ImVec2(fx(t.level_db), p.y + h - 2), with_alpha(c, 0.8f), 2.0f);
    }
    if (show_gate) {
        float gx = roundf(fx(t.s.gate_db));
        dl->AddLine(ImVec2(gx, p.y), ImVec2(gx, p.y + h), COL_WARN, 1.0f);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Input level %.0f dBFS (gate %.0f)", t.level_db, t.s.gate_db);
}

static bool toggle_button(const char* label, bool on, ImVec4 on_col) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, on_col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(on_col.x * 1.2f, on_col.y * 1.2f, on_col.z * 1.2f, 1));
    }
    bool r = ImGui::Button(label);
    if (on) ImGui::PopStyleColor(2);
    return r;
}

static void toggle_mic(Tuner& t) { t.mic_on = !t.mic_on; t.restart_capture = true; }

static void shift_octave(Tuner& t, int d) {
    int ns = std::max(-2, std::min(3, t.s.octave_shift + d));
    if (ns != t.s.octave_shift) { t.s.octave_shift = ns; t.settings_dirty = true; }
}

static void toggle_settings(Tuner& t) {
    t.settings_open = !t.settings_open;
    if (t.settings_open) t.devices = capture_device_names();
}

static void draw_toolbar(Tuner& t) {
    const ImVec4 green(0.16f, 0.45f, 0.30f, 1.0f), blue(0.15f, 0.36f, 0.52f, 1.0f);
    if (toggle_button(t.mic_on ? "Mic On" : "Mic Off", t.mic_on, green)) toggle_mic(t);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Listen to the input (Space)");
    ImGui::SameLine();
    level_bar(t, 56.0f, ImGui::GetFrameHeight(), false);

    ImGui::SameLine(0, 12);
    if (ImGui::ArrowButton("##octdn", ImGuiDir_Left)) shift_octave(t, -1);
    ImGui::SameLine(0, 4);
    char rng[32];
    int lo = piano_min(t.s);
    snprintf(rng, sizeof(rng), "C%d-C%d", midi_octave(lo), midi_octave(lo + 36));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(rng);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keyboard range (Up/Down arrows)");
    ImGui::SameLine(0, 4);
    if (ImGui::ArrowButton("##octup", ImGuiDir_Right)) shift_octave(t, +1);

    const ImGuiStyle& st = ImGui::GetStyle();
    float bw = ImGui::CalcTextSize("Pin").x + ImGui::CalcTextSize("Full").x + ImGui::CalcTextSize("Settings").x
             + st.FramePadding.x * 6 + st.ItemSpacing.x * 2;
    float right = ImGui::GetWindowContentRegionMax().x - bw;
    if (right > ImGui::GetCursorPosX()) ImGui::SameLine(right);
    else                                ImGui::SameLine();
    if (toggle_button("Pin", t.s.always_on_top, blue)) { t.s.always_on_top = !t.s.always_on_top; t.settings_dirty = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the window above other windows");
    ImGui::SameLine();
    if (ImGui::Button("Full")) platform_toggle_fullscreen(t.window);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fullscreen (F)");
    ImGui::SameLine();
    if (toggle_button("Settings", t.settings_open, blue)) toggle_settings(t);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings (Cmd+,)");
}

// ---------------------------------------------------------------------------
// Settings window (non-modal)

static void draw_settings(Tuner& t) {
    if (!t.settings_open) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8, vp->WorkPos.y + 32),
                            ImGuiCond_FirstUseEver, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.97f);
    bool open = true;
    if (!ImGui::Begin("Settings", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) { t.settings_open = false; t.settings_dirty = true; }
        return;
    }
    Settings& s = t.s;
    bool ch = false;
    const float W = 170.0f;
    auto sliderf = [&](const char* label, float* v, float lo, float hi, const char* fmt, const char* tip) {
        ImGui::SetNextItemWidth(W);
        bool c = ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
        ch |= c;
    };

    ImGui::SeparatorText("Input");
    ImGui::SetNextItemWidth(W);
    const char* cur = s.input_device.empty() ? "System default" : s.input_device.c_str();
    if (ImGui::BeginCombo("Device", cur)) {
        if (ImGui::Selectable("System default", s.input_device.empty())) {
            s.input_device.clear(); ch = true; t.restart_capture = true;
        }
        for (const std::string& d : t.devices)
            if (ImGui::Selectable(d.c_str(), d == s.input_device)) {
                s.input_device = d; ch = true; t.restart_capture = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemClicked()) t.devices = capture_device_names();
    level_bar(t, W, 8.0f, true);
    ImGui::SameLine();
    ImGui::TextDisabled("level");
    sliderf("Noise gate", &s.gate_db, -90.0f, -10.0f, "%.0f dBFS", "Input quieter than this reads as silence (yellow mark on the level bar).");

    ImGui::SeparatorText("Detection");
    sliderf("A4", &s.a4, 400.0f, 480.0f, "%.1f Hz", "Reference pitch. Ctrl/Cmd+click to type a value.");
    sliderf("Min frequency", &s.min_freq, 25.0f, 400.0f, "%.0f Hz", "Lowest pitch searched for.");
    sliderf("Max frequency", &s.max_freq, 200.0f, 4000.0f, "%.0f Hz", "Highest pitch searched for.");
    sliderf("YIN threshold", &s.yin_threshold, 0.05f, 0.40f, "%.2f", "Lower is stricter about periodicity; raise it if breathy notes drop out.");
    sliderf("Min confidence", &s.min_confidence, 0.0f, 0.95f, "%.2f", "Estimates below this are ignored.");
    sliderf("Note confidence", &s.note_confidence, 0.0f, 0.99f, "%.2f", "Confidence needed before the displayed note changes. Higher means fewer spurious notes.");
    sliderf("Note settle", &s.note_settle_ms, 0.0f, 300.0f, "%.0f ms", "How long a new note must be heard before it is shown.");
    sliderf("Note hysteresis", &s.note_hysteresis, 0.0f, 40.0f, "%.0f cents", "How far past the halfway point between notes the pitch must go before the note changes.");
    sliderf("Smoothing", &s.smoothing, 0.0f, 0.95f, "%.2f", "Steadies the reading; higher is calmer but slower.");
    sliderf("Hold", &s.hold_ms, 0.0f, 1000.0f, "%.0f ms", "Keep showing the last pitch through brief dropouts.");
    {
        static const int SIZES[] = { 2048, 4096, 8192 };
        static const char* NAMES[] = { "2048 (fast)", "4096", "8192 (low notes)" };
        int idx = s.window_size == 2048 ? 0 : s.window_size == 8192 ? 2 : 1;
        ImGui::SetNextItemWidth(W);
        if (ImGui::Combo("Window", &idx, NAMES, 3)) { s.window_size = SIZES[idx]; ch = true; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Analysis window in samples. Larger is steadier and reaches lower, but lags more.");
    }

    ImGui::SeparatorText("Display");
    sliderf("In tune", &s.in_tune_cents, 1.0f, 25.0f, "\xC2\xB1%.0f cents", "Green zone.");
    sliderf("Close", &s.close_cents, 2.0f, 50.0f, "\xC2\xB1%.0f cents", "Yellow zone; beyond is red.");
    sliderf("History", &s.history_s, 2.0f, 60.0f, "%.0f s", nullptr);
    ch |= ImGui::Checkbox("Show history", &s.show_history);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("Key labels", &s.show_key_labels);
    ch |= ImGui::Checkbox("Always on top", &s.always_on_top);

    ImGui::SeparatorText("Reference tone");
    ImGui::SetNextItemWidth(W);
    ch |= ImGui::Combo("Waveform", &s.tone_wave, TONE_WAVE_NAMES, WAVE_COUNT);
    sliderf("Volume", &s.tone_volume, 0.0f, 1.0f, "%.2f", nullptr);

    ImGui::Separator();
    if (ImGui::Button("Defaults")) {
        Settings d;
        d.input_device = s.input_device;
        d.win_x = s.win_x; d.win_y = s.win_y; d.win_w = s.win_w; d.win_h = s.win_h;
        d.octave_shift = s.octave_shift;
        d.always_on_top = s.always_on_top;
        s = d;
        ch = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keys\n"
                          "  Z..M , . /   lower octave + (S D G H J L ;)\n"
                          "  Q..P [ ]     upper octaves + (2 3 4 6 7 9 0 -)\n"
                          "  Up / Down    shift keyboard octave\n"
                          "  Space        mic on/off\n"
                          "  F, Cmd+F     fullscreen (Esc leaves)\n"
                          "  Cmd+,        settings");
    ImGui::End();

    if (ch) { settings_clamp(&s); t.settings_dirty = true; }
    if (!open) { t.settings_open = false; t.settings_dirty = true; }
}

// ---------------------------------------------------------------------------
// Keyboard

static void handle_keys(Tuner& t) {
    ImGuiIO& io = ImGui::GetIO();
    bool cmd = io.KeyCtrl || io.KeySuper;   // Cmd on macOS (ImGui swaps), Ctrl elsewhere

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Comma)) toggle_settings(t);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) platform_toggle_fullscreen(t.window);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) t.quit = true;

    if (io.WantTextInput || cmd || io.KeyAlt) {
        // Typing into a field: release anything held from the keyboard.
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_mic(t);
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) platform_toggle_fullscreen(t.window);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (t.fullscreen) platform_toggle_fullscreen(t.window);
        else if (t.settings_open) { t.settings_open = false; t.settings_dirty = true; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))   shift_octave(t, +1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) shift_octave(t, -1);

    int lo = piano_min(t.s);
    for (int i = 0; i < PIANO_N; i++) {
        if (ImGui::IsKeyPressed(PIANO_KEYS[i], false) && s_key_note[i] < 0) {
            s_key_note[i] = lo + i;
            held_remove(t, lo + i);
            t.held.push_back(lo + i);
        }
    }
}

// Key releases are tracked even while modifiers are down, so a note never sticks.
static void handle_key_releases(Tuner& t) {
    for (int i = 0; i < PIANO_N; i++) {
        if (s_key_note[i] >= 0 && !ImGui::IsKeyDown(PIANO_KEYS[i])) {
            held_remove(t, s_key_note[i]);
            s_key_note[i] = -1;
        }
    }
}

// ---------------------------------------------------------------------------

void ui_frame(Tuner& t, double now) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 5));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(COL_BG));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (!t.fullscreen) {
        draw_toolbar(t);
    }
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec2 wmax(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    float W = wmax.x - vp->WorkPos.x, H = wmax.y - c0.y;
    float sc = std::max(1.0f, std::min(3.0f, std::min(W / 520.0f, (H + 30.0f) / 400.0f)));
    float pad = roundf(6.0f * sc);
    float x = vp->WorkPos.x + pad, w = W - 2 * pad;
    float y = (t.fullscreen ? vp->WorkPos.y : c0.y) + (t.fullscreen ? pad * 2 : 0);
    float y_end = wmax.y - pad;
    float avail = y_end - y;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float piano_h = std::min(std::max(w * 0.12f, 44.0f), avail * 0.28f);
    float meter_h = std::min(std::max(avail * 0.09f, 24.0f), 110.0f);
    float readout_h;
    bool  hist = t.s.show_history;
    if (hist) {
        readout_h = std::min(std::max(avail * (t.fullscreen ? 0.30f : 0.24f), 48.0f), 420.0f);
        if (avail - readout_h - meter_h - piano_h - 3 * pad < 24.0f) hist = false;
    }
    if (!hist) readout_h = avail - meter_h - piano_h - 2 * pad;
    readout_h = std::max(readout_h, 30.0f);

    draw_readout(t, dl, x, y, w, readout_h, sc);
    if (!t.mic_on || audio_error()[0]) {
        const char* msg = audio_error()[0] ? audio_error() : "mic off - press Space";
        text_at(dl, 12.0f * sc, ImVec2(x, y), audio_error()[0] ? COL_BAD : COL_DIM, msg);
    }
    y += readout_h + pad;
    draw_meter(t, dl, x, y, w, meter_h, sc);
    y += meter_h + pad;

    PianoGeom g;
    float piano_y = y_end - piano_h;
    g.init(x, piano_y, w, piano_h, piano_min(t.s));
    if (hist) draw_history(t, dl, g, y, piano_y - 1 - y, now, sc);
    draw_piano(t, dl, g);

    // Mouse playing: click and drag across keys.
    ImGui::SetCursorScreenPos(ImVec2(g.x, g.y));
    ImGui::InvisibleButton("##piano", ImVec2(g.w, g.h));
    if (ImGui::IsItemActive()) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        int m = g.hit(mp.x, mp.y);
        if (m != t.mouse_note) {
            if (t.mouse_note >= 0) held_remove(t, t.mouse_note);
            t.mouse_note = m;
            if (m >= 0) t.held.push_back(m);
        }
    } else if (t.mouse_note >= 0) {
        held_remove(t, t.mouse_note);
        t.mouse_note = -1;
    }

    if (t.fullscreen && now - t.fs_entered_t < 3.0) {
        float a = (float)std::min(1.0, 3.0 - (now - t.fs_entered_t));
        text_centered(dl, 14.0f * sc, vp->WorkPos.x + W * 0.5f, vp->WorkPos.y + 4, with_alpha(COL_MUTED, a),
                      "F or Esc to leave fullscreen");
    }
    ImGui::End();

    draw_settings(t);
    handle_keys(t);
    handle_key_releases(t);
    ui_set_tone(t);
}
