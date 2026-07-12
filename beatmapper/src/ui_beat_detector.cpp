#include "ui_beat_detector.h"
#include "imgui.h"
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
    s_needs_run = false;
    (void)editor;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ui_beat_detector_ensure_onsets(AudioState* audio, BeatMap* beatmap,
                                    AutoBeatList* autobeat,
                                    double t1, double t2)
{
    // If onset data already fully covers the requested range, nothing to do.
    if (autobeat->onset_count > 0 &&
        t1 >= s_last_t_start && t2 <= s_last_t_end)
        return;
    run_detection(nullptr, audio, beatmap, autobeat, t1, t2);
}

void ui_beat_detector_render(EditorState* editor, AudioState* audio,
                             BeatMap* beatmap, UndoStack* undo,
                             AutoBeatList* autobeat)
{
    if (!editor->show_beat_detector) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 280), ImVec2(500, 700));
    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Beat Detector", &editor->show_beat_detector,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    float avail_w = ImGui::GetContentRegionAvail().x;

    // --- Algorithm selector ---
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

    ImGui::Spacing();

    // --- Parameters ---
    float half_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // Min / Max BPM on one row
    ImGui::SetNextItemWidth(half_w);
    if (ImGui::SliderFloat("##minbpm", &s_min_bpm, 30.0f, 180.0f, "Min %.0f"))
        s_needs_run = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum expected tempo (BPM)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half_w);
    if (ImGui::SliderFloat("##maxbpm", &s_max_bpm, 60.0f, 300.0f, "Max %.0f"))
        s_needs_run = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum expected tempo (BPM)");

    // Onset threshold
    ImGui::SetNextItemWidth(avail_w);
    if (ImGui::SliderFloat("##thresh", &s_threshold, 0.5f, 5.0f, "Thresh %.2f"))
        s_needs_run = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Onset sensitivity: mean + N * std deviation (lower = more sensitive)");

    // DP tightness
    ImGui::SetNextItemWidth(avail_w);
    if (ImGui::SliderFloat("##tight", &s_tightness, 10.0f, 2000.0f, "Tight %.0f", ImGuiSliderFlags_Logarithmic))
        s_needs_run = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ellis DP tightness: higher = stricter tempo adherence");

    // Pre-onset shift
    ImGui::SetNextItemWidth(avail_w);
    if (ImGui::SliderFloat("##prems", &s_pre_onset_ms, 0.0f, 100.0f, "Pre %.0f ms"))
        s_needs_run = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shift each beat this many ms before the onset peak\n"
                          "Places the marker in the quiet moment before the attack");

    ImGui::Spacing();

    // --- Options ---
    if (ImGui::Checkbox("Use accepted beats", &s_use_seeds))
        s_needs_run = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Seed tempo and phase from accepted beats already in the window");

    if (ImGui::Checkbox("Show raw onsets", &editor->show_raw_onsets)) { /* immediate effect */ }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show unregularised onset ticks in the Auto strip");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Hybrid interpolation:");
    ImGui::Spacing();

    ImGui::Checkbox("Snap shift+click to onsets", &editor->snap_interp_to_onsets);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "When shift+clicking to fill beats, each grid position is pulled\n"
            "to the nearest detected onset (within \xc2\xb1" "20%% of the beat period).\n"
            "The BPM grid anchors the rhythm; audio snaps the fine placement.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Determine analysis window ---
    bool   have_window = false;
    double t_start = 0.0, t_end = 0.0;
    if (editor->has_region) {
        t_start = editor->region_start;
        t_end   = editor->region_end;
        have_window = (audio_pcm_data(audio, nullptr, nullptr, nullptr) != nullptr);
    }

    // --- Auto-run when window or params change ---
    if (have_window && (params_changed(t_start, t_end) || s_needs_run))
        run_detection(editor, audio, beatmap, autobeat, t_start, t_end);
    else if (!have_window) {
        // Clear stale results when no window
        autobeat->beat_count  = 0;
        autobeat->onset_count = 0;
        autobeat->estimated_bpm = 0.0f;
        s_last_t_start = s_last_t_end = -99.0;
        s_last_algo    = -1;
    }

    // --- Manual detect button ---
    bool detect_enabled = have_window;
    if (!detect_enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Detect Now", ImVec2(avail_w, 0))) {
        s_needs_run = true;
        if (have_window)
            run_detection(editor, audio, beatmap, autobeat, t_start, t_end);
    }
    if (!detect_enabled) ImGui::EndDisabled();

    ImGui::Spacing();

    // --- Status ---
    if (!have_window) {
        ImGui::TextDisabled("(select a region to detect beats)");
    } else if (autobeat->beat_count > 0) {
        if (autobeat->estimated_bpm > 0.0f)
            ImGui::TextDisabled("Detected %d beats  ~%.1f BPM",
                                autobeat->beat_count, autobeat->estimated_bpm);
        else
            ImGui::TextDisabled("Detected %d beats", autobeat->beat_count);
    } else {
        ImGui::TextDisabled("No beats detected");
    }

    ImGui::Spacing();

    // --- Selection / insertion controls ---
    bool have_beats = (autobeat->beat_count > 0);
    if (!have_beats) ImGui::BeginDisabled();

    // Count selected
    int n_sel = 0;
    for (int i = 0; i < autobeat->beat_count; i++)
        if (autobeat->beat_selected[i]) n_sel++;

    float btn_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    if (ImGui::Button("Select All", ImVec2(btn_w, 0))) {
        for (int i = 0; i < autobeat->beat_count; i++)
            autobeat->beat_selected[i] = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Select None", ImVec2(btn_w, 0))) {
        for (int i = 0; i < autobeat->beat_count; i++)
            autobeat->beat_selected[i] = false;
    }

    bool can_insert = (n_sel > 0);
    if (!can_insert) ImGui::BeginDisabled();
    if (ImGui::Button("Insert Selected", ImVec2(avail_w, 0))) {
        undo_push(undo, beatmap, nullptr);
        for (int i = 0; i < autobeat->beat_count; i++)
            if (autobeat->beat_selected[i])
                beatmap_add(beatmap, autobeat->beat_times[i]);
        // Remove inserted beats from autobeat list
        int j = 0;
        for (int i = 0; i < autobeat->beat_count; i++)
            if (!autobeat->beat_selected[i]) {
                autobeat->beat_times[j]    = autobeat->beat_times[i];
                autobeat->beat_selected[j] = false;
                j++;
            }
        autobeat->beat_count = j;
    }
    if (!can_insert) ImGui::EndDisabled();

    if (!have_beats) ImGui::EndDisabled();

    if (ImGui::Button("Clear", ImVec2(avail_w, 0))) {
        autobeat->beat_count  = 0;
        autobeat->onset_count = 0;
        s_last_t_start = s_last_t_end = -99.0;
        s_last_algo    = -1;
    }

    ImGui::End();
}
