#include "ui_chroma.h"
#include "chroma_algo.h"
#include "imgui.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Note names
// ---------------------------------------------------------------------------
static const char* NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// ---------------------------------------------------------------------------
// Chroma colormap:
//   0–10%   black (noise floor)
//   10–25%  fade up to faint purple
//   25–50%  purple → darkish blue (transition zone)
//   50–75%  darkish blue gradient
//   75%     sharp jump to green (two stops bracketing 75%)
//   75–100% dark green → bright green gradient
// ---------------------------------------------------------------------------
static ImU32 chroma_colormap(float v)
{
    struct Stop { float t; int r, g, b; };
    static const Stop stops[] = {
        { 0.00f,   0,   0,   0 },   //   0%  black
        { 0.10f,  10,   5,  14 },   //  10%  near-black (darkness cutoff)
        { 0.25f,  60,  22,  80 },   //  25%  faint purple
        { 0.50f,  28,  55, 150 },   //  50%  darkish blue
        { 0.74f,  35,  90, 195 },   //  74%  vivid blue  (top of blue zone)
        { 0.76f,  18, 150,  45 },   //  76%  dark green  (bottom of green zone)
        { 1.00f,  55, 230,  85 },   // 100%  bright green
    };
    static const int N = 7;
    if (v <= 0.0f) return IM_COL32(stops[0].r,   stops[0].g,   stops[0].b,   255);
    if (v >= 1.0f) return IM_COL32(stops[N-1].r, stops[N-1].g, stops[N-1].b, 255);
    for (int i = 0; i < N - 1; i++) {
        if (v < stops[i+1].t) {
            float f = (v - stops[i].t) / (stops[i+1].t - stops[i].t);
            int r = (int)(stops[i].r + f * (stops[i+1].r - stops[i].r) + 0.5f);
            int g = (int)(stops[i].g + f * (stops[i+1].g - stops[i].g) + 0.5f);
            int b = (int)(stops[i].b + f * (stops[i+1].b - stops[i].b) + 0.5f);
            return IM_COL32(r, g, b, 255);
        }
    }
    return IM_COL32(stops[N-1].r, stops[N-1].g, stops[N-1].b, 255);
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static float s_chroma[12]      = {};
static double s_last_t_start   = -99.0;
static double s_last_t_end     = -99.0;
static int    s_last_algo      = -1;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ui_chroma_render(EditorState* editor, AudioState* audio)
{
    if (!editor->show_chroma_panel) {
        editor->chroma_hover_note = -1;
        return;
    }

    // Persistent UI state
    static int   s_algo_idx      = 0;
    static float s_roll_secs     = 2.0f;

    // Fetch PCM
    uint64_t frame_count = 0;
    uint32_t channels    = 0;
    uint32_t sample_rate = 0;
    const float* pcm = audio_pcm_data(audio, &frame_count, &channels, &sample_rate);

    // Determine analysis window
    double t_start = 0.0, t_end = 0.0;
    bool   have_window = false;

    if (pcm) {
        if (editor->has_region) {
            t_start     = editor->region_start;
            t_end       = editor->region_end;
            have_window = true;
        } else if (audio->playing) {
            t_end       = audio->position;
            t_start     = t_end - (double)s_roll_secs;
            have_window = true;
        }
    }

    // Recompute when window or algorithm changes
    if (have_window && (t_start  != s_last_t_start ||
                        t_end    != s_last_t_end   ||
                        s_algo_idx != s_last_algo)) {
        CHROMA_ALGOS[s_algo_idx].fn(pcm, frame_count, channels, sample_rate,
                                     t_start, t_end, s_chroma);
        s_last_t_start = t_start;
        s_last_t_end   = t_end;
        s_last_algo    = s_algo_idx;
    } else if (!have_window) {
        memset(s_chroma, 0, sizeof(s_chroma));
        s_last_t_start = s_last_t_end = -99.0;
        s_last_algo    = -1;
    }

    // --- Floating window ---
    ImGui::SetNextWindowSizeConstraints(ImVec2(120, 220), ImVec2(500, 800));
    ImGui::SetNextWindowSize(ImVec2(200, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Chroma Analyzer", &editor->show_chroma_panel,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        editor->chroma_hover_note = -1;
        return;
    }

    float avail_w = ImGui::GetContentRegionAvail().x;

    // --- Algorithm selector ---
    ImGui::SetNextItemWidth(avail_w);
    // Lambda-style item getter for the Combo
    struct AlgoGetter {
        static bool get(void* /*data*/, int idx, const char** out_text) {
            if (idx < 0 || idx >= CHROMA_ALGO_COUNT) return false;
            *out_text = CHROMA_ALGOS[idx].name;
            return true;
        }
    };
    if (ImGui::Combo("##algo", &s_algo_idx, AlgoGetter::get, nullptr, CHROMA_ALGO_COUNT)) {
        // Force recompute on next frame
        s_last_t_start = s_last_t_end = -99.0;
        s_last_algo    = -1;
    }
    if (ImGui::IsItemHovered() && s_algo_idx >= 0 && s_algo_idx < CHROMA_ALGO_COUNT)
        ImGui::SetTooltip("%s", CHROMA_ALGOS[s_algo_idx].tip);

    // --- Rolling window size (shown only when not using a region) ---
    if (!editor->has_region) {
        ImGui::SetNextItemWidth(avail_w);
        if (ImGui::SliderFloat("##win", &s_roll_secs, 0.5f, 10.0f, "roll %.1fs")) {
            s_last_t_start = s_last_t_end = -99.0;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rolling window duration (seconds)");
    }

    ImGui::Spacing();

    // --- Status line ---
    if (!pcm)
        ImGui::TextDisabled("(no audio)");
    else if (!have_window)
        ImGui::TextDisabled("(select region or play)");
    else if (editor->has_region)
        ImGui::TextDisabled("%.2fs – %.2fs", t_start, t_end);
    else
        ImGui::TextDisabled("rolling  %.2fs", t_end);

    ImGui::Spacing();

    // --- Vertical bar layout ---
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec2      avail  = ImGui::GetContentRegionAvail();
    ImVec2      origin = ImGui::GetCursorScreenPos();

    const float LBL_W   = 26.0f;
    const float GAP     =  2.0f;
    const float total_h = avail.y;
    float bar_h = (total_h - 11.0f * GAP) / 12.0f;
    if (bar_h < 13.0f) bar_h = 13.0f;

    ImGui::Dummy(ImVec2(avail.x, 12.0f * (bar_h + GAP) - GAP));

    editor->chroma_hover_note = -1;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float  lh    = ImGui::GetTextLineHeight();

    for (int i = 0; i < 12; i++) {
        int   note = 11 - i;   // row 0 = B (highest), row 11 = C (lowest)
        float y0  = origin.y + (float)i * (bar_h + GAP);
        float y1  = y0 + bar_h;
        float bx0 = origin.x + LBL_W;
        float bx1 = origin.x + avail.x;

        bool hov = (mouse.x >= origin.x && mouse.x < bx1 &&
                    mouse.y >= y0        && mouse.y <  y1);
        if (hov) editor->chroma_hover_note = note;

        // Note label
        ImU32 lbl_col = hov ? IM_COL32(220, 255, 220, 255)
                            : IM_COL32(170, 178, 170, 210);
        float lbl_y = y0 + (bar_h - lh) * 0.5f;
        dl->AddText(ImVec2(origin.x + 2.0f, lbl_y), lbl_col, NOTE_NAMES[note]);

        // Colored bar
        ImU32 fill   = chroma_colormap(s_chroma[note]);
        dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(bx1, y1), fill, 3.0f);

        // Border
        ImU32 border = hov ? IM_COL32(160, 255, 160, 220)
                           : IM_COL32(40, 55, 40, 130);
        dl->AddRect(ImVec2(bx0, y0), ImVec2(bx1, y1), border, 3.0f);
    }

    if (editor->chroma_hover_note >= 0)
        ImGui::SetTooltip("%s  %.0f%%",
                          NOTE_NAMES[editor->chroma_hover_note],
                          s_chroma[editor->chroma_hover_note] * 100.0f);

    ImGui::End();
}
