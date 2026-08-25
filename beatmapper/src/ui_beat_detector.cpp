#include "ui_beat_detector.h"
#include "ui_complete.h"
#include "imgui.h"
#include <stdio.h>
#include "onset_shape.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Persistent state
// ---------------------------------------------------------------------------
static int   s_algo_idx       = 0;
static float s_min_bpm        = 60.0f;
static float s_max_bpm        = 200.0f;
static float s_threshold      = 1.5f;
static float s_tightness      = 400.0f;
static float s_pre_onset_ms   = 0.0f;
static bool  s_use_seeds      = true;   // incorporate accepted beats as seeds
static bool  s_needs_run      = false;  // force re-detection on next frame

// Last-seen params (to detect changes and trigger re-detection)
static double s_last_t_start  = -99.0;
static double s_last_t_end    = -99.0;
static int    s_last_algo     = -1;
static float  s_last_min_bpm  = -1.0f;
static float  s_last_max_bpm  = -1.0f;
static float  s_last_thresh   = -1.0f;
static float  s_last_tight    = -1.0f;
static float  s_last_pre_ms   = -1.0f;
static bool   s_last_seeds    = false;
static bool   s_last_from_region = false;  // last run analysed the editor region

// Seed buffer (accepted beats within the window)
static double s_seed_buf[MAX_BEAT_CANDS];
static int    s_seed_count = 0;

// ---------------------------------------------------------------------------
// Helper: returns true if any params changed since last run
// ---------------------------------------------------------------------------
static bool params_changed(double t_start, double t_end) {
    return (t_start     != s_last_t_start  ||
            t_end       != s_last_t_end    ||
            s_algo_idx  != s_last_algo     ||
            s_min_bpm   != s_last_min_bpm  ||
            s_max_bpm   != s_last_max_bpm  ||
            s_threshold != s_last_thresh   ||
            s_tightness != s_last_tight    ||
            s_pre_onset_ms != s_last_pre_ms||
            s_use_seeds != s_last_seeds);
}

static void save_last(double t_start, double t_end) {
    s_last_t_start  = t_start;
    s_last_t_end    = t_end;
    s_last_algo     = s_algo_idx;
    s_last_min_bpm  = s_min_bpm;
    s_last_max_bpm  = s_max_bpm;
    s_last_thresh   = s_threshold;
    s_last_tight    = s_tightness;
    s_last_pre_ms   = s_pre_onset_ms;
    s_last_seeds    = s_use_seeds;
}

// ---------------------------------------------------------------------------
// Run detection
// ---------------------------------------------------------------------------
static void run_detection(EditorState* editor, AudioState* audio,
                          BeatMap* beatmap, AutoBeatList* autobeat,
                          double t_start, double t_end)
{
    uint64_t frame_count = 0;
    uint32_t channels    = 0;
    uint32_t sample_rate = 0;
    const float* pcm = audio_pcm_data(audio, &frame_count, &channels, &sample_rate);
    if (!pcm) return;

    // Collect accepted beats within the window as seeds
    s_seed_count = 0;
    if (s_use_seeds) {
        for (int i = 0; i < beatmap->count && s_seed_count < MAX_BEAT_CANDS; i++) {
            double bt = beatmap->beats[i].time;
            if (bt >= t_start && bt <= t_end)
                s_seed_buf[s_seed_count++] = bt;
        }
    }

    BeatAlgoParams p = {};
    p.min_bpm         = s_min_bpm;
    p.max_bpm         = s_max_bpm;
    p.onset_threshold = s_threshold;
    p.dp_tightness    = s_tightness;
    p.pre_onset_ms    = s_pre_onset_ms;
    p.seed_times      = (s_seed_count >= 2) ? s_seed_buf : nullptr;
    p.seed_count      = (s_seed_count >= 2) ? s_seed_count : 0;

    BEAT_ALGOS[s_algo_idx].fn(pcm, frame_count, channels, sample_rate,
                               t_start, t_end, &p, autobeat);
    save_last(t_start, t_end);

    // Timbre of each detected beat, against the track vocabulary when one
    // has been trained (else unclassified boxes that still show the window).
    // Skipped while the Complete Track worker owns the shape singleton -- the
    // classify pass reads the vocabulary the worker may be retraining.
    if (!ui_complete_analysis_running()) {
        AudioPcm a = { pcm, frame_count, channels, sample_rate };
        double win = shape_track()->win;
        if (win <= 0.0) {
            double per = autobeat->estimated_bpm > 0 ? 60.0 / autobeat->estimated_bpm : 0.5;
            win = shape_params()->window_frac * per;
        }
        shape_marks_classify(SHAPE_SRC_DETECTED, a, autobeat->beat_times, autobeat->beat_count, win);
    }
    s_needs_run = false;
    (void)editor;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ui_beat_detector_reset(AutoBeatList* autobeat) {
    autobeat->beat_count    = 0;
    autobeat->onset_count   = 0;
    autobeat->estimated_bpm = 0.0f;
    s_last_t_start = s_last_t_end = -99.0;
    s_last_algo    = -1;
    s_needs_run    = false;
    s_last_from_region = false;
    shape_marks_clear(SHAPE_SRC_DETECTED);
}

void ui_beat_detector_update(EditorState* editor, AutoBeatList* autobeat,
                             bool tool_visible)
{
    if (!editor->has_region) {
        if (autobeat->beat_count > 0 || autobeat->onset_count > 0 || s_last_algo != -1)
            ui_beat_detector_reset(autobeat);
        return;
    }
    // Hidden tool: results for a window other than the current region are
    // stale ghosts, not something the user asked for.
    if (!tool_visible && s_last_from_region &&
        (editor->region_start != s_last_t_start || editor->region_end != s_last_t_end))
        ui_beat_detector_reset(autobeat);
}

void ui_beat_detector_ensure_onsets(AudioState* audio, BeatMap* beatmap,
                                    AutoBeatList* autobeat,
                                    double t1, double t2)
{
    // If onset data already fully covers the requested range, nothing to do.
    if (autobeat->onset_count > 0 &&
        t1 >= s_last_t_start && t2 <= s_last_t_end)
        return;
    run_detection(nullptr, audio, beatmap, autobeat, t1, t2);
    s_last_from_region = false;
}

// The analysis window the body last settled on, for the actions row.
static bool   s_have_window = false;
static double s_win_t0 = 0.0, s_win_t1 = 0.0;

void ui_beat_detector_settings(ToolCtx& c)
{
    EditorState* editor = c.editor;
    float avail_w = ImGui::GetContentRegionAvail().x;

    ImGui::SetNextItemWidth(avail_w);
    struct AlgoGetter {
        static bool get(void*, int idx, const char** out_text) {
            if (idx < 0 || idx >= BEAT_ALGO_COUNT) return false;
            *out_text = BEAT_ALGOS[idx].name;
            return true;
        }
    };
    if (ImGui::Combo("##algo", &s_algo_idx, AlgoGetter::get, nullptr, BEAT_ALGO_COUNT))
        s_needs_run = true;
    if (ImGui::IsItemHovered() && s_algo_idx >= 0 && s_algo_idx < BEAT_ALGO_COUNT)
        ImGui::SetTooltip("%s", BEAT_ALGOS[s_algo_idx].tip);

    float half_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth(half_w);
    if (ImGui::SliderFloat("##minbpm", &s_min_bpm, 30.0f, 180.0f, "Min %.0f")) s_needs_run = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum expected tempo (BPM)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half_w);
    if (ImGui::SliderFloat("##maxbpm", &s_max_bpm, 60.0f, 300.0f, "Max %.0f")) s_needs_run = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum expected tempo (BPM)");

    ImGui::SetNextItemWidth(avail_w);
    if (ImGui::SliderFloat("##thresh", &s_threshold, 0.5f, 5.0f, "Thresh %.2f")) s_needs_run = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Onset sensitivity: mean + N * std deviation (lower = more sensitive)");
    ImGui::SetNextItemWidth(avail_w);
    if (ImGui::SliderFloat("##tight", &s_tightness, 10.0f, 2000.0f, "Tight %.0f", ImGuiSliderFlags_Logarithmic))
        s_needs_run = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ellis DP tightness: higher = stricter tempo adherence");
    ImGui::SetNextItemWidth(avail_w);
    if (ImGui::SliderFloat("##prems", &s_pre_onset_ms, 0.0f, 100.0f, "Pre %.0f ms")) s_needs_run = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shift each beat this many ms before the onset peak\n"
                          "Places the marker in the quiet moment before the attack");

    if (ImGui::Checkbox("Use accepted beats", &s_use_seeds)) s_needs_run = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seed tempo and phase from accepted beats already in the window");
    ImGui::Checkbox("Show raw onsets", &editor->show_raw_onsets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show unregularised onset ticks in the Auto strip");
    ImGui::Checkbox("Snap shift+click to onsets", &editor->snap_interp_to_onsets);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "When shift+clicking to fill beats, each grid position is pulled\n"
            "to the nearest detected onset (within \xc2\xb1" "20%% of the beat period).\n"
            "The BPM grid anchors the rhythm; audio snaps the fine placement.");
}

void ui_beat_detector_body(ToolCtx& c)
{
    EditorState*  editor   = c.editor;
    AudioState*   audio    = c.audio;
    BeatMap*      beatmap  = c.beatmap;
    AutoBeatList* autobeat = c.autobeat;

    s_have_window = false;
    if (editor->has_region) {
        s_win_t0 = editor->region_start;
        s_win_t1 = editor->region_end;
        s_have_window = (audio_pcm_data(audio, nullptr, nullptr, nullptr) != nullptr);
    }
    if (s_have_window && (params_changed(s_win_t0, s_win_t1) || s_needs_run)) {
        run_detection(editor, audio, beatmap, autobeat, s_win_t0, s_win_t1);
        s_last_from_region = true;
    } else if (!s_have_window)
        ui_beat_detector_reset(autobeat);

    ImGui::TextDisabled("Detection");
    if (!s_have_window) {
        ImGui::TextDisabled("(select a region to detect beats)");
    } else if (autobeat->beat_count > 0) {
        int n_sel = 0;
        for (int i = 0; i < autobeat->beat_count; i++) if (autobeat->beat_selected[i]) n_sel++;
        if (autobeat->estimated_bpm > 0.0f)
            ImGui::Text("%d beats  ~%.1f BPM", autobeat->beat_count, autobeat->estimated_bpm);
        else
            ImGui::Text("%d beats", autobeat->beat_count);
        ImGui::TextDisabled("%d selected; click or drag in the Auto strip, I inserts", n_sel);
    } else {
        ImGui::TextDisabled("No beats detected");
    }
}

void ui_beat_detector_actions(ToolCtx& c)
{
    EditorState*  editor   = c.editor;
    AudioState*   audio    = c.audio;
    BeatMap*      beatmap  = c.beatmap;
    UndoStack*    undo     = c.undo;
    AutoBeatList* autobeat = c.autobeat;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;

    int n_sel = 0;
    for (int i = 0; i < autobeat->beat_count; i++) if (autobeat->beat_selected[i]) n_sel++;
    bool have_beats = autobeat->beat_count > 0;

    // Row 1: Detect | Insert N | Clear
    float w3 = (avail_w - 2.0f * sp) / 3.0f;
    if (!s_have_window) ImGui::BeginDisabled();
    if (ImGui::Button("Detect", ImVec2(w3, 0))) {
        s_needs_run = true;
        if (s_have_window) { run_detection(editor, audio, beatmap, autobeat, s_win_t0, s_win_t1); s_last_from_region = true; }
    }
    if (!s_have_window) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(s_have_window ? "Run detection over the selected region" : "Select a region first");
    ImGui::SameLine();
    if (n_sel == 0) ImGui::BeginDisabled();
    char ib[32]; snprintf(ib, sizeof(ib), "Insert %d", n_sel);
    if (ImGui::Button(ib, ImVec2(w3, 0))) {
        undo_push(undo, beatmap, nullptr);
        for (int i = 0; i < autobeat->beat_count; i++)
            if (autobeat->beat_selected[i]) beatmap_add(beatmap, autobeat->beat_times[i]);
        int j = 0;
        for (int i = 0; i < autobeat->beat_count; i++)
            if (!autobeat->beat_selected[i]) {
                autobeat->beat_times[j]    = autobeat->beat_times[i];
                autobeat->beat_selected[j] = false;
                j++;
            }
        autobeat->beat_count = j;
    }
    if (n_sel == 0) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!have_beats) ImGui::BeginDisabled();
    if (ImGui::Button("Clear", ImVec2(w3, 0))) ui_beat_detector_reset(autobeat);
    if (!have_beats) ImGui::EndDisabled();

    // Row 2: Select all | Select none
    float w2 = (avail_w - sp) * 0.5f;
    if (!have_beats) ImGui::BeginDisabled();
    if (ImGui::Button("Select all", ImVec2(w2, 0)))
        for (int i = 0; i < autobeat->beat_count; i++) autobeat->beat_selected[i] = true;
    ImGui::SameLine();
    if (ImGui::Button("Select none", ImVec2(w2, 0)))
        for (int i = 0; i < autobeat->beat_count; i++) autobeat->beat_selected[i] = false;
    if (!have_beats) ImGui::EndDisabled();
}
