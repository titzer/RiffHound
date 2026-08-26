#include "ui_smoothing.h"
#include "ui_beat_detector.h"
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

// Recompute the proposed positions for beats [i0, i1].
static void preview_compute(const BeatMap* bm, int i0, int i1,
                            const AutoBeatList* ab)
{
    int n = i1 - i0 + 1;
    if (n > MAX_SMOOTH) n = MAX_SMOOTH;

    for (int k = 0; k < n; k++) {
        s_orig[k] = bm->beats[i0 + k].time;
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

    s_have_preview   = true;
    s_preview.i0     = i0;
    s_preview.i1     = i0 + n - 1;
    s_preview.n      = n;
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
    if (!ui_smoothing_can_accept()) return;
    undo_push(undo, beatmap, lyricmap, sectionmap, miscmap, chordmap);
    beatmap_retime_annotations(sectionmap, lyricmap, miscmap, chordmap,
                               s_orig, s_prop, s_preview.n, PIN_TOL);
    beatmap_apply_times(beatmap, s_preview.i0, s_prop, s_preview.n);
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

    int i0 = -1, i1 = -1;
    int  n_sel     = beatmap_selection_range(beatmap, &i0, &i1);
    int  n_full    = (i0 >= 0) ? (i1 - i0 + 1) : 0;
    int  n_range   = (n_full > MAX_SMOOTH) ? MAX_SMOOTH : n_full;
    bool capped    = (n_full > MAX_SMOOTH);
    bool have_range = (n_range >= 3);
    if (have_range) i1 = i0 + n_range - 1;
    s_i0 = i0; s_i1 = i1; s_n_sel = n_sel; s_n_range = n_range; s_have_range = have_range;

    if (n_sel == 0) {
        ImGui::TextDisabled("Select a range of beats, or just a start\nand an end beat, in the Beats strip.");
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
// accidental double-time (halve), accidental half-time (subdivide).

void ui_beats_edit_actions(ToolCtx& c)
{
    BeatMap* bm = c.beatmap;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float sp      = ImGui::GetStyle().ItemSpacing.x;

    std::vector<int> sel;
    for (int i = 0; i < bm->count; i++)
        if (bm->beats[i].selected) sel.push_back(i);
    const int n_sel = (int)sel.size();

    // Interval at beat i, from its neighbours (before any mutation).
    auto local_iv = [&](int i) {
        if (i + 1 < bm->count) return bm->beats[i + 1].time - bm->beats[i].time;
        if (i > 0)             return bm->beats[i].time - bm->beats[i - 1].time;
        return 0.5;
    };

    auto shift_sel = [&](double frac) {
        undo_push(c.undo, bm, c.lyricmap, c.sectionmap, c.miscmap, c.chordmap);
        std::vector<double> oldt(n_sel), newt(n_sel);
        for (int k = 0; k < n_sel; k++) {
            oldt[k] = bm->beats[sel[k]].time;
            newt[k] = oldt[k] + frac * local_iv(sel[k]);
        }
        // Annotation edges pinned to the moved beats follow them.
        beatmap_retime_annotations(c.sectionmap, c.lyricmap, c.miscmap, c.chordmap,
                                   oldt.data(), newt.data(), n_sel, 1e-4);
        for (int k = 0; k < n_sel; k++) bm->beats[sel[k]].time = newt[k];
        std::sort(bm->beats, bm->beats + bm->count,
                  [](const Beat& a, const Beat& b) { return a.time < b.time; });
        bm->dirty = true;
    };

    auto halve = [&](bool keep_first) {
        undo_push(c.undo, bm, c.lyricmap);
        // Drop every other selected beat; descending keeps indices valid.
        for (int k = n_sel - 1; k >= 0; k--)
            if ((k % 2) == (keep_first ? 1 : 0))
                beatmap_remove(bm, sel[k]);
    };

    // Row 1: shift by half a beat either way
    bool can1 = n_sel >= 1;
    float half_w = (avail_w - sp) * 0.5f;
    if (!can1) ImGui::BeginDisabled();
    if (ImGui::Button("Shift -\xc2\xbd beat", ImVec2(half_w, 0))) shift_sel(-0.5);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move every selected beat back by half its local interval\n"
                          "(annotation edges pinned to those beats follow)");
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
        undo_push(c.undo, bm, c.lyricmap);
        for (int k = n_sel - 1; k > 0; k--) {
            int i = sel[k - 1], j = sel[k];
            if (j != i + 1) continue;          // only pairs adjacent in the map
            double mid = 0.5 * (bm->beats[i].time + bm->beats[j].time);
            int idx = beatmap_add(bm, mid);
            if (idx >= 0) {
                bm->beats[idx].interp   = true;
                bm->beats[idx].selected = true;
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
    if (ImGui::Button("Accept smoothing (S)", ImVec2(half_w, 0)))
        ui_smoothing_accept(beatmap, c.undo, c.sectionmap, c.lyricmap,
                            c.miscmap, c.chordmap);
    ImGui::PopStyleColor(3);
    if (!can_apply) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(can_apply ? "Move the beats to the previewed positions (Ctrl+Z undoes)"
                                    : "Select at least 3 beats to smooth");
    ImGui::SameLine();
    bool any_sel = beatmap_selected_count(beatmap) > 0;
    if (!any_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Clear selection", ImVec2(half_w, 0))) {
        beatmap_clear_selection(beatmap);
        preview_clear();
    }
    if (!any_sel) ImGui::EndDisabled();
}
