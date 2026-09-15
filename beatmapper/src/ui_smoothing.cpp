#include "ui_smoothing.h"
#include "ui_beat_detector.h"
#include "ui_timeline.h"
#include "imgui.h"
#include <math.h>
#include <string.h>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Persistent state
// ---------------------------------------------------------------------------
static const int MAX_SMOOTH = 4096;   // beats a single preview can cover

static SmoothParams s_p;
static bool         s_p_init      = false;
static bool         s_show_ghosts = true;

static double s_orig[MAX_SMOOTH];
static double s_prop[MAX_SMOOTH];
static SmoothPreview s_preview = { false, -1, -1, 0, s_orig, s_prop };

// Signature of the inputs the preview was computed from.
struct PreviewKey {
    int    i0, i1, beat_count, onset_count;
    double checksum;      // detects beats edited elsewhere (drag, undo, fill)
    float  strength, onset_weight, onset_window, max_shift;
    int    iterations;
    bool   use_onsets;
};
static PreviewKey s_key;
static bool       s_key_valid   = false;
static bool       s_have_preview = false;   // proposal computed and up to date

static BpmStats s_before, s_after;
static double   s_max_shift_s = 0.0;   // largest |delta| in the preview
static double   s_mean_shift_s = 0.0;

const SmoothPreview* ui_smoothing_preview() { return &s_preview; }
const SmoothParams*  ui_smoothing_params() {
    if (!s_p_init) { smooth_params_defaults(&s_p); s_p_init = true; }
    return &s_p;
}

static bool key_equal(const PreviewKey& a, const PreviewKey& b) {
    return a.i0 == b.i0 && a.i1 == b.i1 &&
           a.beat_count == b.beat_count && a.onset_count == b.onset_count &&
           a.checksum == b.checksum &&
           a.strength == b.strength && a.iterations == b.iterations &&
           a.use_onsets == b.use_onsets && a.onset_weight == b.onset_weight &&
           a.onset_window == b.onset_window && a.max_shift == b.max_shift;
}

static void preview_clear() {
    s_preview.active = false;
    s_preview.i0 = s_preview.i1 = -1;
    s_preview.n  = 0;
    s_key_valid    = false;
    s_have_preview = false;
    s_before = s_after = BpmStats{ 0, 0.0, 0.0, 0.0, 0.0 };
    s_max_shift_s = s_mean_shift_s = 0.0;
}

// Recompute the proposed positions for an arbitrary chronological time list
// (map beats, taps, detected beats, or a mix of them).
static void preview_compute_times(const double* times, int n, const AutoBeatList* ab)
{
    if (n > MAX_SMOOTH) n = MAX_SMOOTH;

    for (int k = 0; k < n; k++) {
        s_orig[k] = times[k];
        s_prop[k] = s_orig[k];
    }

    beat_smooth_times(s_prop, n, &s_p,
                      (s_p.use_onsets && ab) ? ab->onset_times : nullptr,
                      (s_p.use_onsets && ab) ? ab->onset_count : 0);

    s_before = beatmap_bpm_stats(s_orig, n);
    s_after  = beatmap_bpm_stats(s_prop, n);

    s_max_shift_s  = 0.0;
    s_mean_shift_s = 0.0;
    for (int k = 0; k < n; k++) {
        double d = fabs(s_prop[k] - s_orig[k]);
        if (d > s_max_shift_s) s_max_shift_s = d;
        s_mean_shift_s += d;
    }
    if (n > 0) s_mean_shift_s /= n;

    s_have_preview = true;
    s_preview.i0   = -1;
    s_preview.i1   = -1;
    s_preview.n    = n;
}

// Recompute the proposed positions for beats [i0, i1].
static void preview_compute(const BeatMap* bm, int i0, int i1,
                            const AutoBeatList* ab)
{
    int n = i1 - i0 + 1;
    if (n > MAX_SMOOTH) n = MAX_SMOOTH;
    static std::vector<double> tmp;
    tmp.resize((size_t)n);
    for (int k = 0; k < n; k++) tmp[k] = bm->beats[i0 + k].time;
    preview_compute_times(tmp.data(), n, ab);
    s_preview.i0 = i0;
    s_preview.i1 = i0 + n - 1;
}

// --- combined selection ----------------------------------------------------
// The tool's selection-based operations treat every selected beat-like item
// the same, wherever it lives: the beat map, the tap strip, or the detector's
// list.  A MixRef remembers where each one came from so results write back.

struct MixRef { int src; int idx; };   // src: 0 = map beat, 1 = tap, 2 = detected

static std::vector<MixRef> s_mix;      // refs parallel to the mixed preview
static bool                s_mix_mode = false;

static void autobeat_sort(AutoBeatList* ab) {
    if (!ab) return;
    for (int a = 1; a < ab->beat_count; a++)
        for (int b = a; b > 0 && ab->beat_times[b] < ab->beat_times[b - 1]; b--) {
            std::swap(ab->beat_times[b],    ab->beat_times[b - 1]);
            std::swap(ab->beat_selected[b], ab->beat_selected[b - 1]);
        }
}

static int gather_selection(ToolCtx& c, std::vector<MixRef>* refs,
                            std::vector<double>* times,
                            int* n_map, int* n_tap, int* n_ab)
{
    struct Item { double t; MixRef r; };
    std::vector<Item> v;
    *n_map = *n_tap = *n_ab = 0;
    for (int i = 0; i < c.beatmap->count; i++)
        if (c.beatmap->beats[i].selected) { v.push_back({ c.beatmap->beats[i].time, { 0, i } }); (*n_map)++; }
    int ntap = 0;
    TapEntry* taps = ui_timeline_taps(&ntap);
    for (int i = 0; i < ntap; i++)
        if (taps[i].selected) { v.push_back({ taps[i].time, { 1, i } }); (*n_tap)++; }
    if (c.autobeat)
        for (int i = 0; i < c.autobeat->beat_count; i++)
            if (c.autobeat->beat_selected[i]) { v.push_back({ c.autobeat->beat_times[i], { 2, i } }); (*n_ab)++; }
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return a.t < b.t; });
    refs->clear(); times->clear();
    for (const Item& it : v) { refs->push_back(it.r); times->push_back(it.t); }
    return (int)v.size();
}

// Endpoints of annotations are treated as "on a beat" within this tolerance.
// Beats are never closer than 5 ms, and the file format keeps 6 decimals.
static const double PIN_TOL = 1e-4;

// How many section / lyric / chord / misc endpoints are pinned to a beat the preview
// moves — i.e. how many annotations Accept would drag along.
static int count_pinned_one(const double* orig, const double* prop, int n, double t) {
    if (n <= 0) return 0;
    int lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; if (orig[m] < t) lo = m + 1; else hi = m; }
    int best = -1;
    double bd = 0.0;
    if (lo < n)  { best = lo; bd = orig[lo] - t; }
    if (lo > 0) { double d = t - orig[lo - 1]; if (best < 0 || d < bd) { best = lo - 1; bd = d; } }
    if (best < 0 || bd > PIN_TOL) return 0;
    return (fabs(prop[best] - orig[best]) > 1e-12) ? 1 : 0;
}

static int count_pinned(const SectionMap* sm, const LyricMap* lm, const MiscMap* mm,
                        const MiscMap* cm,
                        const double* orig, const double* prop, int n)
{
    int c = 0;
    if (sm) for (int i = 0; i < sm->count; i++) {
        c += count_pinned_one(orig, prop, n, sm->sections[i].t_start);
        c += count_pinned_one(orig, prop, n, sm->sections[i].t_end);
    }
    if (lm) for (int i = 0; i < lm->count; i++) {
        c += count_pinned_one(orig, prop, n, lm->lyrics[i].t_start);
        c += count_pinned_one(orig, prop, n, lm->lyrics[i].t_end);
    }
    const MiscMap* anns[2] = { mm, cm };
    for (int a = 0; a < 2; a++) {
        if (!anns[a]) continue;
        for (int i = 0; i < anns[a]->count; i++) {
            c += count_pinned_one(orig, prop, n, anns[a]->entries[i].t_start);
            c += count_pinned_one(orig, prop, n, anns[a]->entries[i].t_end);
        }
    }
    return c;
}

// Count onsets that fall inside the range (informational).
static int onsets_in_range(const AutoBeatList* ab, double t0, double t1) {
    if (!ab) return 0;
    int n = 0;
    for (int i = 0; i < ab->onset_count; i++)
        if (ab->onset_times[i] >= t0 && ab->onset_times[i] <= t1) n++;
    return n;
}

void ui_smoothing_hidden() {
    // Panel not on screen this frame: don't leave stale ghosts behind.
    preview_clear();
}

bool ui_smoothing_can_accept() {
    return s_preview.n > 0 && s_max_shift_s > 1e-6;
}

void ui_smoothing_accept(BeatMap* beatmap, UndoStack* undo, SectionMap* sectionmap,
                         LyricMap* lyricmap, MiscMap* miscmap, MiscMap* chordmap)
{
    if (!ui_smoothing_can_accept() || s_preview.i0 < 0) return;
    undo_push(undo, beatmap, lyricmap, sectionmap, miscmap, chordmap);
    beatmap_retime_annotations(sectionmap, lyricmap, miscmap, chordmap,
                               s_orig, s_prop, s_preview.n, PIN_TOL);
    beatmap_apply_times(beatmap, s_preview.i0, s_prop, s_preview.n);
    s_key_valid = false;
}

// Accept for a mixed selection: each smoothed time writes back to wherever
// its item lives (map / taps / detected), map beats dragging their pinned
// annotations along as usual.
static void accept_mixed(ToolCtx& c)
{
    if (!ui_smoothing_can_accept() || !s_mix_mode) return;
    if ((int)s_mix.size() < s_preview.n) return;
    undo_push(c.undo, c.beatmap, c.lyricmap, c.sectionmap, c.miscmap, c.chordmap, c.taps);
    std::vector<double> mo, mn;
    for (int k = 0; k < s_preview.n; k++)
        if (s_mix[k].src == 0) { mo.push_back(s_orig[k]); mn.push_back(s_prop[k]); }
    if (!mo.empty())
        beatmap_retime_annotations(c.sectionmap, c.lyricmap, c.miscmap, c.chordmap,
                                   mo.data(), mn.data(), (int)mo.size(), PIN_TOL);
    int ntap = 0;
    TapEntry* taps = ui_timeline_taps(&ntap);
    for (int k = 0; k < s_preview.n; k++) {
        const MixRef& r = s_mix[k];
        switch (r.src) {
        case 0: if (r.idx < c.beatmap->count) { c.beatmap->beats[r.idx].time = s_prop[k]; c.beatmap->dirty = true; } break;
        case 1: if (r.idx < ntap) taps[r.idx].time = s_prop[k]; break;
        case 2: if (c.autobeat && r.idx < c.autobeat->beat_count) c.autobeat->beat_times[r.idx] = s_prop[k]; break;
        }
    }
    // The smoother preserves order within the selection, but a moved item can
    // cross an unselected neighbour in its own store: re-sort each store.
    std::sort(c.beatmap->beats, c.beatmap->beats + c.beatmap->count,
              [](const Beat& a, const Beat& b) { return a.time < b.time; });
    ui_timeline_taps_sort();
    autobeat_sort(c.autobeat);
    s_key_valid = false;
}

// Selection range the body last settled on, for the actions row.
static int  s_i0 = -1, s_i1 = -1, s_n_sel = 0, s_n_range = 0;
static bool s_have_range = false;

static void smoothing_presets(float avail_w) {
    struct Preset { const char* name; const char* tip; float strength; int iters; float max_shift; };
    static const Preset PRESETS[3] = {
        { "Light",  "Nudge each beat a little toward its neighbours\n(strength 0.25, 5 passes, 20 ms max)",   0.25f,  5, 0.020f },
        { "Medium", "Even out local jitter\n(strength 0.5, 20 passes, 50 ms max)",                          0.50f, 20, 0.050f },
        { "Strong", "Converge toward a constant tempo across the range\n(strength 0.8, 100 passes, 150 ms max)", 0.80f, 100, 0.150f },
    };
    float third_w = (avail_w - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    for (int k = 0; k < 3; k++) {
        const Preset& pr = PRESETS[k];
        bool active = (s_p.strength == pr.strength && s_p.iterations == pr.iters &&
                       s_p.max_shift == pr.max_shift);
        if (k) ImGui::SameLine();
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.37f, 0.63f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.47f, 0.75f, 1.0f));
        }
        if (ImGui::Button(pr.name, ImVec2(third_w, 0))) {
            s_p.strength   = pr.strength;
            s_p.iterations = pr.iters;
            s_p.max_shift  = pr.max_shift;
            s_key_valid    = false;
        }
        if (active) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pr.tip);
    }
}

void ui_smoothing_settings(ToolCtx& c)
{
    (void)c;
    if (!s_p_init) { smooth_params_defaults(&s_p); s_p_init = true; }
    float w = ImGui::GetContentRegionAvail().x;
    smoothing_presets(w);
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##strength", &s_p.strength, 0.02f, 1.0f, "Strength %.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far each beat moves toward the midpoint of its\nneighbours on every pass");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##iters", &s_p.iterations, 1, 200, "Passes %d", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("More passes spread the correction further and converge\ntoward a constant tempo across the range");
    {
        float shift_ms = s_p.max_shift * 1000.0f;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##maxshift", &shift_ms, 0.0f, 300.0f,
                               shift_ms <= 0.0f ? "Max shift: unlimited" : "Max shift %.0f ms"))
            s_p.max_shift = shift_ms / 1000.0f;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hard limit on how far any single beat may move from\nits current position (0 = no limit)");
    }
    ImGui::Checkbox("Use detected onsets", &s_p.use_onsets);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pull smoothed beats toward onsets found by the detector,\nso the result follows the audio as well as the tempo");
    if (!s_p.use_onsets) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##opull", &s_p.onset_weight, 0.0f, 1.0f, "Onset pull %.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = ignore onsets, 1 = snap onto them");
    ImGui::SetNextItemWidth(w);
    {
        float win_pct = s_p.onset_window * 100.0f;
        if (ImGui::SliderFloat("##owin", &win_pct, 2.0f, 50.0f, "Search \xc2\xb1%.0f%% of beat"))
            s_p.onset_window = win_pct / 100.0f;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Onsets further than this from the beat are ignored");
    if (!s_p.use_onsets) ImGui::EndDisabled();
    ImGui::Checkbox("Show preview on timeline", &s_show_ghosts);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the proposed positions of beats that would move as\nhollow markers with a line back to where each is now");
    if (ImGui::Button("Reset knobs", ImVec2(w, 0))) {
        smooth_params_defaults(&s_p);
        s_key_valid = false;
    }
}

void ui_smoothing_body(ToolCtx& c)
{
    if (!s_p_init) { smooth_params_defaults(&s_p); s_p_init = true; }
    AudioState*   audio    = c.audio;
    BeatMap*      beatmap  = c.beatmap;
    AutoBeatList* autobeat = c.autobeat;
    float avail_w = ImGui::GetContentRegionAvail().x;

    ImGui::TextDisabled("Smoothing");

    // Combined selection: map beats, taps, detected beats.  With only map
    // beats selected, the classic semantics apply (the selection's index
    // range, unselected interior beats included); as soon as taps or
    // detected beats are in the selection, exactly the selected items are
    // smoothed together.
    static std::vector<double> s_mix_t;
    int n_map = 0, n_tap = 0, n_ab = 0;
    gather_selection(c, &s_mix, &s_mix_t, &n_map, &n_tap, &n_ab);
    s_mix_mode = (n_tap + n_ab) > 0;

    if (s_mix_mode) {
        int  n = (int)s_mix_t.size();
        bool capped = n > MAX_SMOOTH;
        if (capped) n = MAX_SMOOTH;
        bool have_range = n >= 3;
        s_i0 = s_i1 = -1; s_n_sel = n; s_n_range = n; s_have_range = have_range;

        ImGui::Text("%d selected (%d map, %d tap%s, %d detected)",
                    n, n_map, n_tap, n_tap == 1 ? "" : "s", n_ab);
        if (!have_range)
            ImGui::TextDisabled("Need at least 3 selected.");
        if (capped)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Capped at %d.", MAX_SMOOTH);

        if (s_p.use_onsets && have_range) {
            double t0 = s_mix_t.front(), t1 = s_mix_t[n - 1];
            int n_on = onsets_in_range(autobeat, t0, t1);
            if (n_on == 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "No onsets in range.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Detect in range")) {
                    ui_beat_detector_ensure_onsets(audio, beatmap, autobeat, t0, t1);
                    s_key_valid = false;
                }
            } else {
                ImGui::TextDisabled("%d onsets in range", n_on);
            }
        }

        if (have_range) {
            double checksum = 0.0;
            for (int k = 0; k < n; k++) checksum += s_mix_t[k] * (k + 1);
            PreviewKey key;
            key.i0 = -1; key.i1 = n;
            key.beat_count   = n_map * 1000000 + n_tap * 1000 + n_ab;
            key.onset_count  = s_p.use_onsets && autobeat ? autobeat->onset_count : 0;
            key.checksum     = checksum;
            key.strength     = s_p.strength;
            key.iterations   = s_p.iterations;
            key.use_onsets   = s_p.use_onsets;
            key.onset_weight = s_p.onset_weight;
            key.onset_window = s_p.onset_window;
            key.max_shift    = s_p.max_shift;
            if (!s_key_valid || !key_equal(key, s_key)) {
                preview_compute_times(s_mix_t.data(), n, autobeat);
                s_key = key; s_key_valid = true;
            }
        } else {
            preview_clear();
        }
        s_preview.active = s_have_preview && s_show_ghosts;
    } else {

    int i0 = -1, i1 = -1;
    int  n_sel     = beatmap_selection_range(beatmap, &i0, &i1);
    int  n_full    = (i0 >= 0) ? (i1 - i0 + 1) : 0;
    int  n_range   = (n_full > MAX_SMOOTH) ? MAX_SMOOTH : n_full;
    bool capped    = (n_full > MAX_SMOOTH);
    bool have_range = (n_range >= 3);
    if (have_range) i1 = i0 + n_range - 1;
    s_i0 = i0; s_i1 = i1; s_n_sel = n_sel; s_n_range = n_range; s_have_range = have_range;

    if (n_sel == 0) {
        ImGui::TextDisabled("Select any beats -- mapped, tapped or detected --\n"
                            "in their strips.  A map-only selection smooths the\n"
                            "whole index range between its endpoints.");
    } else if (!have_range) {
        ImGui::TextDisabled("Selected %d beat%s spanning %d \xe2\x80\x94 need at least 3.",
                            n_sel, n_sel == 1 ? "" : "s", n_full);
    } else {
        ImGui::Text("Beats %d\xe2\x80\x93%d  (%d beats, %.2fs)", i0, i1, n_range,
                    beatmap->beats[i1].time - beatmap->beats[i0].time);
        ImGui::TextDisabled("%d selected, %d interior beats may move", n_sel, n_range - 2);
        if (capped)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Range capped at %d beats.", MAX_SMOOTH);
    }

    // Onset availability + (re-)detection over the range.
    if (s_p.use_onsets && have_range) {
        double t0 = beatmap->beats[i0].time;
        double t1 = beatmap->beats[i1].time;
        int n_on = onsets_in_range(autobeat, t0, t1);
        if (n_on == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "No onsets in range.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Detect in range")) {
                ui_beat_detector_ensure_onsets(audio, beatmap, autobeat, t0, t1);
                s_key_valid = false;
            }
        } else {
            ImGui::TextDisabled("%d onsets in range", n_on);
        }
    }

    // --- Preview -----------------------------------------------------------
    if (have_range) {
        double checksum = 0.0;
        for (int k = 0; k < n_range; k++) checksum += beatmap->beats[i0 + k].time * (k + 1);
        PreviewKey key;
        key.i0 = i0; key.i1 = i0 + n_range - 1;
        key.beat_count   = beatmap->count;
        key.onset_count  = s_p.use_onsets && autobeat ? autobeat->onset_count : 0;
        key.checksum     = checksum;
        key.strength     = s_p.strength;
        key.iterations   = s_p.iterations;
        key.use_onsets   = s_p.use_onsets;
        key.onset_weight = s_p.onset_weight;
        key.onset_window = s_p.onset_window;
        key.max_shift    = s_p.max_shift;
        if (!s_key_valid || !key_equal(key, s_key)) {
            preview_compute(beatmap, i0, i0 + n_range - 1, autobeat);
            s_key = key; s_key_valid = true;
        }
    } else {
        preview_clear();
    }
    s_preview.active = s_have_preview && s_show_ghosts;

    }  // end map-only path

    if (s_preview.n > 0) {
        if (ImGui::BeginTable("##sm_stats", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("now");
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("after");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Mean BPM");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", s_before.mean);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", s_after.mean);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Std dev");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", s_before.stddev);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(s_after.stddev <= s_before.stddev
                                   ? ImVec4(0.55f, 0.95f, 0.55f, 1.0f) : ImVec4(1.00f, 0.60f, 0.40f, 1.0f),
                               "%.2f", s_after.stddev);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("BPM range");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f\xe2\x80\x93%.1f", s_before.min_bpm, s_before.max_bpm);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f\xe2\x80\x93%.1f", s_after.min_bpm, s_after.max_bpm);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Beat shift: %.1f ms max, %.1f ms average",
                            s_max_shift_s * 1000.0, s_mean_shift_s * 1000.0);
        int n_pinned = count_pinned(c.sectionmap, c.lyricmap, c.miscmap, c.chordmap,
                                    s_orig, s_prop, s_preview.n);
        if (n_pinned > 0)
            ImGui::TextDisabled("%d annotation edge%s pinned to a moved beat will follow",
                                n_pinned, n_pinned == 1 ? "" : "s");
    }
    (void)avail_w;
}

// --- Selection range edits -------------------------------------------------
// The classic rescue jobs: a stretch mapped half a beat off (shift +-1/2),
// accidental double-time (halve), accidental half-time (subdivide).  Works
// on the combined selection: mapped beats, taps and detected beats alike,
// each edit landing back in the item's own store.

void ui_beats_edit_actions(ToolCtx& c)
{
    BeatMap*      bm = c.beatmap;
    AutoBeatList* ab = c.autobeat;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float sp      = ImGui::GetStyle().ItemSpacing.x;

    std::vector<MixRef> sel;
    std::vector<double> selt;
    int n_map = 0, n_tap = 0, n_ab = 0;
    const int n_sel = gather_selection(c, &sel, &selt, &n_map, &n_tap, &n_ab);

    int ntap = 0;
    TapEntry* taps = ui_timeline_taps(&ntap);

    // Local interval of an item, from its neighbours in its own store
    // (computed before any mutation).
    auto item_iv = [&](const MixRef& r, double t) -> double {
        switch (r.src) {
        case 0:
            if (r.idx + 1 < bm->count) return bm->beats[r.idx + 1].time - t;
            if (r.idx > 0)             return t - bm->beats[r.idx - 1].time;
            break;
        case 1:
            if (r.idx + 1 < ntap) return taps[r.idx + 1].time - t;
            if (r.idx > 0)        return t - taps[r.idx - 1].time;
            break;
        case 2:
            if (ab) {
                if (r.idx + 1 < ab->beat_count) return ab->beat_times[r.idx + 1] - t;
                if (r.idx > 0)                  return t - ab->beat_times[r.idx - 1];
            }
            break;
        }
        return 0.5;
    };

    auto set_time = [&](const MixRef& r, double t) {
        switch (r.src) {
        case 0: if (r.idx < bm->count) { bm->beats[r.idx].time = t; bm->dirty = true; } break;
        case 1: if (r.idx < ntap) taps[r.idx].time = t; break;
        case 2: if (ab && r.idx < ab->beat_count) ab->beat_times[r.idx] = t; break;
        }
    };

    auto resort_all = [&]() {
        std::sort(bm->beats, bm->beats + bm->count,
                  [](const Beat& a, const Beat& b) { return a.time < b.time; });
        ui_timeline_taps_sort();
        autobeat_sort(ab);
    };

    auto shift_sel = [&](double frac) {
        undo_push(c.undo, bm, c.lyricmap, c.sectionmap, c.miscmap, c.chordmap, c.taps);
        std::vector<double> newt(n_sel);
        for (int k = 0; k < n_sel; k++)
            newt[k] = selt[k] + frac * item_iv(sel[k], selt[k]);
        // Annotation edges pinned to moved MAP beats follow them.
        std::vector<double> mo, mn;
        for (int k = 0; k < n_sel; k++)
            if (sel[k].src == 0) { mo.push_back(selt[k]); mn.push_back(newt[k]); }
        if (!mo.empty())
            beatmap_retime_annotations(c.sectionmap, c.lyricmap, c.miscmap, c.chordmap,
                                       mo.data(), mn.data(), (int)mo.size(), 1e-4);
        for (int k = 0; k < n_sel; k++) set_time(sel[k], newt[k]);
        resort_all();
    };

    auto halve = [&](bool keep_first) {
        undo_push(c.undo, bm, c.lyricmap, nullptr, nullptr, nullptr, c.taps);
        std::vector<int> rm_map, rm_tap, rm_ab;
        for (int k = 0; k < n_sel; k++) {
            if ((k % 2) != (keep_first ? 1 : 0)) continue;
            if (sel[k].src == 0)      rm_map.push_back(sel[k].idx);
            else if (sel[k].src == 1) rm_tap.push_back(sel[k].idx);
            else                      rm_ab.push_back(sel[k].idx);
        }
        std::sort(rm_map.rbegin(), rm_map.rend());
        std::sort(rm_tap.rbegin(), rm_tap.rend());
        std::sort(rm_ab.rbegin(),  rm_ab.rend());
        for (int i : rm_map) beatmap_remove(bm, i);
        for (int i : rm_tap) ui_timeline_tap_remove(i);
        if (ab)
            for (int i : rm_ab) {
                for (int j = i; j + 1 < ab->beat_count; j++) {
                    ab->beat_times[j]    = ab->beat_times[j + 1];
                    ab->beat_selected[j] = ab->beat_selected[j + 1];
                }
                ab->beat_count--;
            }
    };

    // True when any item -- of any store -- lies strictly between t0 and t1,
    // in which case a midpoint insertion would collide with it.
    auto blocked_between = [&](double t0, double t1) {
        double lo = t0 + 1e-6, hi = t1 - 1e-6;
        int a = 0, b2 = bm->count;
        while (a < b2) { int m = (a + b2) / 2; if (bm->beats[m].time <= lo) a = m + 1; else b2 = m; }
        if (a < bm->count && bm->beats[a].time < hi) return true;
        for (int i = 0; i < ntap; i++)
            if (taps[i].time > lo && taps[i].time < hi) return true;
        if (ab)
            for (int i = 0; i < ab->beat_count; i++)
                if (ab->beat_times[i] > lo && ab->beat_times[i] < hi) return true;
        return false;
    };

    auto ab_insert = [&](double t) {
        if (!ab || ab->beat_count >= MAX_BEAT_CANDS) return;
        int pos = 0;
        while (pos < ab->beat_count && ab->beat_times[pos] < t) pos++;
        for (int j = ab->beat_count; j > pos; j--) {
            ab->beat_times[j]    = ab->beat_times[j - 1];
            ab->beat_selected[j] = ab->beat_selected[j - 1];
        }
        ab->beat_times[pos] = t;
        ab->beat_selected[pos] = true;
        ab->beat_count++;
    };

    // Row 1: shift by half a beat either way
    bool can1 = n_sel >= 1;
    float half_w = (avail_w - sp) * 0.5f;
    if (!can1) ImGui::BeginDisabled();
    if (ImGui::Button("Shift -\xc2\xbd beat", ImVec2(half_w, 0))) shift_sel(-0.5);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move every selected beat -- mapped, tapped or detected --\n"
                          "back by half its local interval (annotation edges pinned\n"
                          "to moved map beats follow)");
    ImGui::SameLine();
    if (ImGui::Button("Shift +\xc2\xbd beat", ImVec2(half_w, 0))) shift_sel(0.5);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move every selected beat forward by half its local interval");
    if (!can1) ImGui::EndDisabled();

    // Row 2: subdivide / halve
    bool can2 = n_sel >= 2;
    float third_w = (avail_w - 2.0f * sp) / 3.0f;
    if (!can2) ImGui::BeginDisabled();
    if (ImGui::Button("Subdivide \xc3\x97""2", ImVec2(third_w, 0))) {
        undo_push(c.undo, bm, c.lyricmap, nullptr, nullptr, nullptr, c.taps);
        for (int k = n_sel - 1; k > 0; k--) {
            double t0 = selt[k - 1], t1 = selt[k];
            if (t1 - t0 < 1e-3) continue;
            if (blocked_between(t0, t1)) continue;
            double mid = 0.5 * (t0 + t1);
            switch (sel[k - 1].src) {           // midpoint joins the left item's store
            case 0: {
                int idx = beatmap_add(bm, mid);
                if (idx >= 0) { bm->beats[idx].interp = true; bm->beats[idx].selected = true; }
                break;
            }
            case 1: ui_timeline_tap_insert(mid); break;
            case 2: ab_insert(mid); break;
            }
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Insert a beat at the midpoint of every selected pair\n"
                          "(fix an accidental half-time stretch)");
    ImGui::SameLine();
    if (ImGui::Button("Halve (1st)", ImVec2(third_w, 0))) halve(true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Delete every other selected beat, keeping the first\n"
                          "(fix an accidental double-time stretch)");
    ImGui::SameLine();
    if (ImGui::Button("Halve (2nd)", ImVec2(third_w, 0))) halve(false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Delete every other selected beat, keeping the second");
    if (!can2) ImGui::EndDisabled();
}

void ui_smoothing_actions(ToolCtx& c)
{
    BeatMap* beatmap = c.beatmap;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float half_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    bool can_apply = ui_smoothing_can_accept();
    if (!can_apply) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f, 0.45f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.58f, 0.29f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.68f, 0.35f, 1.0f));
    if (ImGui::Button("Accept smoothing (S)", ImVec2(half_w, 0))) {
        if (s_mix_mode)
            accept_mixed(c);
        else
            ui_smoothing_accept(beatmap, c.undo, c.sectionmap, c.lyricmap,
                                c.miscmap, c.chordmap);
    }
    ImGui::PopStyleColor(3);
    if (!can_apply) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(can_apply ? "Move the beats to the previewed positions (Ctrl+Z undoes)"
                                    : "Select at least 3 beats -- mapped, tapped or detected -- to smooth");
    ImGui::SameLine();
    int ntap = 0;
    TapEntry* taps = ui_timeline_taps(&ntap);
    bool any_tap = false, any_ab = false;
    for (int i = 0; i < ntap && !any_tap; i++) any_tap = taps[i].selected;
    if (c.autobeat)
        for (int i = 0; i < c.autobeat->beat_count && !any_ab; i++)
            any_ab = c.autobeat->beat_selected[i];
    bool any_sel = beatmap_selected_count(beatmap) > 0 || any_tap || any_ab;
    if (!any_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Clear selection", ImVec2(half_w, 0))) {
        beatmap_clear_selection(beatmap);
        for (int i = 0; i < ntap; i++) taps[i].selected = false;
        if (c.autobeat)
            for (int i = 0; i < c.autobeat->beat_count; i++)
                c.autobeat->beat_selected[i] = false;
        preview_clear();
    }
    if (!any_sel) ImGui::EndDisabled();
}
