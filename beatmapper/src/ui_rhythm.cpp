#include "ui_rhythm.h"
#include "ui_complete.h"
#include "onset_shape.h"
#include "beat_algo.h"
#include "imgui.h"
#include <vector>
#include <algorithm>

static const ImU32 SHAPE_COLS[SHAPE_MAX_K] = {
    IM_COL32(235,  90,  80, 255), IM_COL32( 90, 170, 255, 255), IM_COL32(110, 220, 120, 255),
    IM_COL32(240, 200,  70, 255), IM_COL32(200, 120, 240, 255), IM_COL32( 80, 220, 210, 255),
    IM_COL32(240, 140,  60, 255), IM_COL32(180, 180, 180, 255),
};
static int s_beat_algo_idx = 0;

unsigned int ui_rhythm_shape_color(int shape) {
    return (shape >= 0 && shape < SHAPE_MAX_K) ? SHAPE_COLS[shape] : IM_COL32(120, 120, 130, 255);
}

static void tip(const char* s) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s); }

void ui_rhythm_settings(ToolCtx& c)
{
    (void)c;
    ShapeParams& sp = *shape_params();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##sk", &sp.k, 2, SHAPE_MAX_K, "%d shapes");
    tip("Vocabulary size: how many kinds of hit the track is assumed to have");
    {
        float pct = sp.window_frac * 100.0f;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##swf", &pct, 5.0f, 60.0f, "Window %.0f%% of beat"))
            sp.window_frac = pct / 100.0f;
    }
    tip("Length of the descriptor window after each onset (the rectangle in the Timbre strip)");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##ssl", &sp.slots_per_beat, 2, 24, "%d slots per beat");
    tip("Rhythm grid resolution; 12 covers straight and triplet feels");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##sb", &sp.bands, 8, 48, "%d bands");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##sf", &sp.frames, 1, 6, "%d sub-frames");
    tip("Temporal resolution inside the window (kick thump-then-decay vs snare burst)");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##slw", &sp.low_weight, 0.0f, 4.0f, "Low (<200 Hz) weight %.1f");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##shw", &sp.high_weight, 0.0f, 4.0f, "High (>4 kHz) weight %.1f");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##sot", &sp.onset_threshold, 0.3f, 3.0f, "Onset thresh %.2f");
    tip("Detector threshold for harvesting onsets (lower than the beat grid's, to keep quiet hats)");
    struct AlgoGetter {
        static bool get(void*, int idx, const char** out) {
            if (idx < 0 || idx >= BEAT_ALGO_COUNT) return false;
            *out = BEAT_ALGOS[idx].name; return true;
        }
    };
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("##balgo", &s_beat_algo_idx, AlgoGetter::get, nullptr, BEAT_ALGO_COUNT);
    tip("Onset detector used to harvest hits");
}

void ui_rhythm_body(ToolCtx& c)
{
    if (ui_complete_analysis_running()) {
        // The Complete Track worker is training/reading the shape singleton.
        ImGui::TextDisabled("Complete Track is analyzing\xe2\x80\xa6");
        return;
    }
    const ShapeAnalysis* sa = shape_track();
    ImGui::Checkbox("Timbre strip", &c.editor->show_timbre_strip);
    tip("Show the classified windows on the timeline");
    if (!sa->vocab.valid) {
        ImGui::TextDisabled(c.beatmap->count >= 2
            ? "Analyze to classify every onset in the track into\n"
              "track-specific timbre shapes, trained on the mapped beats."
            : "Map some beats first: the vocabulary is learned from\nonsets inside mapped stretches.");
        return;
    }
    ImGui::TextDisabled("%d onsets, %.0f ms window, %d shapes",
                        (int)sa->onset_t.size(), sa->win * 1000.0, sa->vocab.k);

    // Where each shape falls relative to the mapped beats
    const BeatMap* bm = c.beatmap;
    std::vector<int> on(sa->vocab.k, 0), half(sa->vocab.k, 0), other(sa->vocab.k, 0), n(sa->vocab.k, 0);
    std::vector<float> conf(sa->vocab.k, 0.0f);
    for (size_t i = 0; i < sa->onset_t.size(); i++) {
        int sh = sa->onset_shape[i];
        if (sh < 0 || sh >= sa->vocab.k) continue;
        n[sh]++; conf[sh] += sa->onset_soft[i * sa->vocab.k + sh];
        double t = sa->onset_t[i];
        int j = (int)(std::lower_bound(bm->beats, bm->beats + bm->count, t,
                      [](const Beat& b, double v) { return b.time < v; }) - bm->beats);
        if (j <= 0 || j >= bm->count) continue;
        double ph = (t - bm->beats[j - 1].time) / (bm->beats[j].time - bm->beats[j - 1].time);
        if (ph < 0.1 || ph > 0.9) on[sh]++; else if (ph > 0.4 && ph < 0.6) half[sh]++; else other[sh]++;
    }
    if (ImGui::BeginTable("##shapes", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("shape"); ImGui::TableSetupColumn("hits");
        ImGui::TableSetupColumn("dB");    ImGui::TableSetupColumn("on beat");
        ImGui::TableSetupColumn("half");  ImGui::TableSetupColumn("other");
        ImGui::TableHeadersRow();
        for (int s = 0; s < sa->vocab.k; s++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + h, p.y + h - 1), SHAPE_COLS[s], 2.0f);
            ImGui::Dummy(ImVec2(h + 4, h));
            ImGui::SameLine();
            ImGui::Text("%d", s);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("mean membership %.2f", n[s] ? conf[s] / n[s] : 0.0f);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", n[s]);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f", sa->vocab.energy[s]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", on[s]);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%d", half[s]);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%d", other[s]);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("on beat / half: where hits of each shape land\nrelative to the mapped beats");
}

void ui_rhythm_actions(ToolCtx& c)
{
    float w = ImGui::GetContentRegionAvail().x;
    bool loaded = c.audio->loaded && audio_pcm_data(c.audio, nullptr, nullptr, nullptr) &&
                  !ui_complete_analysis_running();
    if (!loaded) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.35f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.45f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
    if (ImGui::Button("Analyze", ImVec2(w, 0))) {
        AudioPcm a;
        a.pcm = audio_pcm_data(c.audio, &a.frame_count, &a.channels, &a.sample_rate);
        ShapeAnalysis* sa = shape_track();
        shape_analysis_ensure(sa, a, c.beatmap, c.audio->duration, *shape_params(), s_beat_algo_idx);
        std::vector<ShapeMark> marks;
        for (size_t i = 0; i < sa->onset_t.size(); i++) {
            ShapeMark m;
            m.t = sa->onset_t[i]; m.win = sa->win;
            m.shape = sa->onset_shape[i]; m.energy = sa->onset_energy[i];
            m.conf = (m.shape >= 0 && sa->vocab.valid) ? sa->onset_soft[i * sa->vocab.k + m.shape] : 0.0f;
            marks.push_back(m);
        }
        shape_marks_set(SHAPE_SRC_ONSET, marks);
        std::vector<double> bt;
        for (int i = 0; i < c.beatmap->count; i++) bt.push_back(c.beatmap->beats[i].time);
        shape_marks_classify(SHAPE_SRC_BEAT, a, bt.data(), (int)bt.size(), sa->win);
        c.editor->show_timbre_strip = true;
    }
    ImGui::PopStyleColor(3);
    if (!loaded) ImGui::EndDisabled();
    tip("Harvest every onset, train the shape vocabulary on the mapped stretches,\n"
        "classify, and show the result in the Timbre strip");
}
