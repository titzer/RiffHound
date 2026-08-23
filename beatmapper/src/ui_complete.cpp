#include "ui_complete.h"
#include "beat_algo.h"
#include "chroma_algo.h"
#include "onset_shape.h"
#include "imgui.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static CompleteParams   s_p;
static bool             s_p_init = false;
static CompleteProposal s_prop;
static BeatChromaCache  s_cache;
static bool             s_have_prop   = false;
static bool             s_show_ghosts = true;
static bool             s_visible     = false;   // rendered this frame
static int              s_hover       = -1;
static double           s_list_r0 = 0.0, s_list_r1 = 0.0;   // region the list is narrowed to
static bool             s_list_narrowed = false;

static const ImU32 COL_DESEL = IM_COL32( 90, 200, 255,  60);
static const ImU32 COL_SEL   = IM_COL32( 90, 200, 255, 170);
static const ImU32 COL_HOVER = IM_COL32(255, 200,  90, 255);

// ---------------------------------------------------------------------------
// Public queries
// ---------------------------------------------------------------------------
const CompleteProposal* ui_complete_proposal() { return &s_prop; }
bool ui_complete_ghosts_active()               { return s_visible && s_have_prop && s_show_ghosts; }
int  ui_complete_hover()                       { return s_hover; }

bool ui_complete_cand_listed(int idx) {
    if (idx < 0 || idx >= (int)s_prop.cands.size()) return false;
    if (!s_list_narrowed) return true;
    return complete_cand_in_range(s_prop.cands[idx], s_list_r0, s_list_r1);
}

unsigned int ui_complete_ghost_color(int idx) {
    if (idx == s_hover) return COL_HOVER;
    if (idx >= 0 && idx < (int)s_prop.cands.size() && s_prop.cands[idx].selected) return COL_SEL;
    return COL_DESEL;
}

void ui_complete_hidden() { s_visible = false; s_hover = -1; }

void ui_complete_reset() {
    s_prop.beat_times.clear(); s_prop.beat_conf.clear();
    s_prop.chords.clear();     s_prop.cands.clear();
    s_prop.onsets.clear();
    s_prop.status[0] = 0;
    s_have_prop = false;
    s_hover = -1;
    beat_chroma_clear(&s_cache);
    shape_analysis_clear(shape_track());
    shape_marks_clear_all();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool settings_header(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(34, 34, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(46, 46, 70, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(56, 56, 84, 255));
    bool open = ImGui::CollapsingHeader(label);
    ImGui::PopStyleColor(3);
    return open;
}

static void tip(const char* s) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s); }

static void run_analysis(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                         SectionMap* sectionmap, MiscMap* chordmap)
{
    CompleteInputs in = {};
    in.beatmap    = beatmap;
    in.sectionmap = sectionmap;
    in.chordmap   = chordmap;
    in.audio.pcm  = audio_pcm_data(audio, &in.audio.frame_count, &in.audio.channels,
                                   &in.audio.sample_rate);
    in.duration   = audio->duration;
    in.has_region = editor->has_region;
    in.region_start = editor->region_start;
    in.region_end   = editor->region_end;
    s_p.shape = *shape_params();          // the Rhythm Map tool owns these
    complete_run(in, s_p, &s_cache, &s_prop);
    s_have_prop = true;
    editor->show_timbre_strip = true;
    s_hover = -1;
}

static void accept_selected(BeatMap* beatmap, SectionMap* sectionmap, LyricMap* lyricmap,
                            MiscMap* miscmap, MiscMap* chordmap, UndoStack* undo)
{
    bool any = false;
    for (const CompleteCand& c : s_prop.cands)
        if (c.selected) { any = true; break; }
    if (!any) return;

    undo_push(undo, beatmap, lyricmap, sectionmap, miscmap, chordmap);
    std::vector<CompleteCand> remaining;
    for (const CompleteCand& c : s_prop.cands) {
        if (!c.selected || !ui_complete_cand_listed((int)(&c - &s_prop.cands[0]))) {
            remaining.push_back(c);
            continue;
        }
        switch (c.kind) {
        case CAND_BEATS:
            for (int i = 0; i < c.n; i++) {
                int k = c.first + i;
                if (k >= 0 && k < (int)s_prop.beat_times.size())
                    beatmap_add(beatmap, s_prop.beat_times[k]);
            }
            break;
        case CAND_SECTION:
            {
                int idx = sectionmap_add(sectionmap, c.t0, c.t1, c.sec_kind, c.label);
                if (idx >= 0) {
                    sectionmap->sections[idx].ts_num = c.ts_num > 0 ? c.ts_num : 4;
                    sectionmap->sections[idx].ts_den = c.ts_den > 0 ? c.ts_den : 4;
                }
                for (int i = 0; i < c.chord_n; i++) {      // the section's chords, as one unit
                    int k = c.chord_first + i;
                    if (k >= 0 && k < (int)s_prop.chords.size())
                        miscmap_add(chordmap, s_prop.chords[k].t0, s_prop.chords[k].t1,
                                    s_prop.chords[k].text);
                }
            }
            break;
        case CAND_CHORDS:
            for (int i = 0; i < c.n; i++) {
                int k = c.first + i;
                if (k >= 0 && k < (int)s_prop.chords.size())
                    miscmap_add(chordmap, s_prop.chords[k].t0, s_prop.chords[k].t1,
                                s_prop.chords[k].text);
            }
            break;
        default: break;
        }
    }
    s_prop.cands.swap(remaining);
    s_hover = -1;
}

// ---------------------------------------------------------------------------
// Tool parts
// ---------------------------------------------------------------------------
static int s_n_listed = 0, s_n_sel = 0, s_n_beat_sel = 0;

static void count_listed() {
    s_n_listed = s_n_sel = s_n_beat_sel = 0;
    for (int i = 0; i < (int)s_prop.cands.size(); i++) {
        if (!ui_complete_cand_listed(i)) continue;
        s_n_listed++;
        if (s_prop.cands[i].selected) {
            s_n_sel++;
            if (s_prop.cands[i].kind == CAND_BEATS) s_n_beat_sel++;
        }
    }
}

void ui_complete_settings(ToolCtx& c)
{
    if (!s_p_init) { complete_params_defaults(&s_p); s_p_init = true; }
    EditorState* editor = c.editor; (void)editor;
    if (settings_header("Beat settings##cp")) {
        ImGui::Indent(6.0f);
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::TextDisabled("Gap-fill strategy:");
        struct FillGetter {
            static bool get(void*, int idx, const char** out) {
                if (idx < 0 || idx >= complete_fill_algo_count()) return false;
                *out = complete_fill_algo_name(idx); return true;
            }
        };
        ImGui::SetNextItemWidth(w);
        ImGui::Combo("##fillalgo", &s_p.fill_algo_idx, FillGetter::get, nullptr, complete_fill_algo_count());
        tip(complete_fill_algo_tip(s_p.fill_algo_idx));
        if (s_p.fill_algo_idx == 1) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##rw", &s_p.rhythm_weight, 0.0f, 2.0f, "Rhythm bonus %.2f");
            tip("Added to the chroma score in proportion to how well the onset shapes match\n"
                "the template's rhythm pattern (0 = chroma only)");
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##mp", &s_p.miss_penalty, 0.0f, 2.0f, "Missing hit penalty %.2f");
            tip("Cost when the pattern expects a hit and the gap has no onset there");
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##ep", &s_p.extra_penalty, 0.0f, 1.0f, "Extra hit penalty %.2f");
            tip("Cost of a clear onset where the pattern expects silence");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Repetition transfer:");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##maxwarp", &s_p.max_warp, 0.0f, 0.25f, "Max stretch %.0f%%",
                           ImGuiSliderFlags_None);
        tip("How far a mapped stretch may be stretched or squeezed to fit");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##warpsteps", &s_p.warp_steps, 1, 15, "Stretch steps %d");
        tip("Stretch factors tried between -max and +max");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##warpw", &s_p.warp_weight, 0.0f, 6.0f, "Warp penalty %.1f");
        tip("Score lost per unit of stretch: 2.0 means an 8%% stretch costs 0.16 similarity");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##bsim", &s_p.beat_sim_threshold, 0.2f, 0.95f, "Transfer threshold %.2f");
        tip("Minimum (similarity - warp penalty) for a transfer; below it the tempo grid is used");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##la", &s_p.lookahead_beats, 0, 16, "Look ahead %d beats");
        tip("A template may start this many whole beats ahead of the current position\n"
            "(the gap is tempo-filled), so a long verse is not pre-empted by a short riff");
        ImGui::SliderFloat("##jit", &s_p.jitter_beats, 0.0f, 2.0f, "Sub-beat slack %.2f");
        tip("Let a template start off the beat grid by up to this much; risks half-beat seams");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##tb", &s_p.template_beats, 4, 64, "Template %d beats");
        tip("Mapped beats outside any section are cut into templates of this length");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##mtb", &s_p.min_template_beats, 2, 16, "Min template %d beats");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##gapf", &s_p.gap_factor, 1.2f, 4.0f, "Gap = %.1fx median beat");
        tip("An interval this many times the median beat counts as unmapped");

        ImGui::Spacing();
        ImGui::TextDisabled("Onsets (Beat Detector):");
        struct AlgoGetter {
            static bool get(void*, int idx, const char** out) {
                if (idx < 0 || idx >= BEAT_ALGO_COUNT) return false;
                *out = BEAT_ALGOS[idx].name; return true;
            }
        };
        ImGui::SetNextItemWidth(w);
        ImGui::Combo("##balgo", &s_p.beat_algo_idx, AlgoGetter::get, nullptr, BEAT_ALGO_COUNT);
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##ow", &s_p.onset_weight, 0.0f, 1.0f, "Onset pull %.2f");
        tip("0 = keep the transferred/grid position, 1 = land on the detected onset");
        {
            float pct = s_p.onset_window * 100.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##owin", &pct, 2.0f, 50.0f, "Search \xc2\xb1%.0f%% of beat"))
                s_p.onset_window = pct / 100.0f;
        }
        float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        ImGui::SliderFloat("##dmin", &s_p.det_min_bpm, 30.0f, 180.0f, "Min %.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        ImGui::SliderFloat("##dmax", &s_p.det_max_bpm, 60.0f, 300.0f, "Max %.0f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##dth", &s_p.det_threshold, 0.5f, 5.0f, "Onset thresh %.2f");

        ImGui::Spacing();
        ImGui::TextDisabled("Smoothing during fill:");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##ss", &s_p.smooth.strength, 0.0f, 1.0f, "Strength %.2f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##si", &s_p.smooth.iterations, 0, 100, "Passes %d");
        {
            float ms = s_p.smooth.max_shift * 1000.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##sms", &ms, 0.0f, 200.0f, ms <= 0 ? "Max shift: unlimited" : "Max shift %.0f ms"))
                s_p.smooth.max_shift = ms / 1000.0f;
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Second-stage smoothing (per segment):");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##s2s", &s_p.smooth2.strength, 0.0f, 1.0f, "Strength %.2f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##s2i", &s_p.smooth2.iterations, 1, 200, "Passes %d", ImGuiSliderFlags_Logarithmic);
        {
            float ms = s_p.smooth2.max_shift * 1000.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##s2ms", &ms, 0.0f, 300.0f, ms <= 0 ? "Max shift: unlimited" : "Max shift %.0f ms"))
                s_p.smooth2.max_shift = ms / 1000.0f;
        }
        ImGui::Checkbox("Use onsets##s2", &s_p.smooth2.use_onsets);
        if (!s_p.smooth2.use_onsets) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##s2ow", &s_p.smooth2.onset_weight, 0.0f, 1.0f, "Onset pull %.2f");
        {
            float pct = s_p.smooth2.onset_window * 100.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##s2owin", &pct, 2.0f, 50.0f, "Search \xc2\xb1%.0f%% of beat"))
                s_p.smooth2.onset_window = pct / 100.0f;
        }
        if (!s_p.smooth2.use_onsets) ImGui::EndDisabled();
        ImGui::Unindent(6.0f);
    }
    if (settings_header("Section settings##cp")) {
        ImGui::Indent(6.0f);
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w);
        ImGui::Combo("##gran", &s_p.section_granularity, "Compare per measure\0Compare per beat\0");
        tip("Granularity of the chroma comparison when matching a section against the track");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##ssim", &s_p.section_sim_threshold, 0.3f, 0.98f, "Match threshold %.2f");
        {
            float pct = s_p.section_max_overlap * 100.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##sov", &pct, 0.0f, 50.0f, "Max overlap %.0f%%"))
                s_p.section_max_overlap = pct / 100.0f;
        }
        tip("How much of a proposed section may overlap one already in the map");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##srw", &s_p.section_rhythm_weight, 0.0f, 1.0f, "Rhythm vs chroma %.2f");
        tip("Blend of the rhythm map (onset shapes per sub-beat) and chroma when comparing passages\n"
            "(also used for chord progressions)");
        ImGui::Checkbox("Discover repeats", &s_p.section_discover);
        tip("Find repeated measure-aligned blocks by self-similarity, even with no section to\n"
            "copy from; groups come out as A, B, C (most repeated = chorus)");
        if (!s_p.section_discover) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##smm", &s_p.section_min_measures, 1, 16, "Min unit %d measures");
        tip("Shortest consecutive repeat discovery will call a section (riffs repeat at 1-2)");
        if (!s_p.section_discover) ImGui::EndDisabled();
        ImGui::Unindent(6.0f);
    }
    if (settings_header("Chord settings##cp")) {
        ImGui::Indent(6.0f);
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::Checkbox("Progression repeats", &s_p.chord_runs);
        tip("Slide each run of mapped chords across the chord-free grid and propose it where\n"
            "every chord sounds (chroma + rhythm) like it does in the map");
        if (!s_p.chord_runs) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##cst", &s_p.chord_sim_threshold, 0.3f, 0.98f, "Match threshold %.2f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##crb", &s_p.chord_run_beats, 4, 128, "Max run %d beats");
        if (!s_p.chord_runs) ImGui::EndDisabled();
        ImGui::Checkbox("Beat-by-beat fallback", &s_p.chord_fallback);
        tip("Where nothing else applies, label beats against what each chord name sounds like\n"
            "in this track (learned from the map; plain triads if there are no chords yet).\n"
            "Noisy; proposals start deselected.");
        if (!s_p.chord_fallback) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##cm", &s_p.chord_margin, 0.02f, 0.5f, "Min margin %.2f");
        tip("Winner must beat the runner-up triad by this much");
        if (!s_p.chord_fallback) ImGui::EndDisabled();
        ImGui::Unindent(6.0f);
    }
    if (settings_header("Chroma per beat##cp")) {
        ImGui::Indent(6.0f);
        float w = ImGui::GetContentRegionAvail().x;
        struct CGetter {
            static bool get(void*, int idx, const char** out) {
                if (idx < 0 || idx >= CHROMA_ALGO_COUNT) return false;
                *out = CHROMA_ALGOS[idx].name; return true;
            }
        };
        ImGui::SetNextItemWidth(w);
        ImGui::Combo("##calgo", &s_p.chroma.algo_idx, CGetter::get, nullptr, CHROMA_ALGO_COUNT);
        if (ImGui::IsItemHovered() && s_p.chroma.algo_idx >= 0 && s_p.chroma.algo_idx < CHROMA_ALGO_COUNT)
            ImGui::SetTooltip("%s", CHROMA_ALGOS[s_p.chroma.algo_idx].tip);
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##att", &s_p.chroma.attack_ms, 0.0f, 200.0f, "Skip attack %.0f ms");
        tip("Ignore the start of each beat, where drum hits smear the spectrum");
        {
            float pct = s_p.chroma.attack_frac * 100.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##attf", &pct, 0.0f, 50.0f, "Skip at least %.0f%% of beat"))
                s_p.chroma.attack_frac = pct / 100.0f;
        }
        tip("The larger of the two skips applies (never more than half the beat)");
        ImGui::TextDisabled("%d beat intervals cached", (int)s_cache.entries.size());
        ImGui::Unindent(6.0f);
    }

}

void ui_complete_body(ToolCtx& c)
{
    if (!s_p_init) { complete_params_defaults(&s_p); s_p_init = true; }
    EditorState* editor  = c.editor;
    BeatMap*     beatmap = c.beatmap;
    s_visible = true;
    s_hover   = -1;

    ImGui::Checkbox("Beats", &s_p.do_beats);
    tip("Fill unmapped stretches: transfer a matching mapped stretch, or continue the tempo");
    ImGui::SameLine();
    ImGui::Checkbox("Sections", &s_p.do_sections);
    tip("Find where mapped sections repeat, and repeats with no template");
    ImGui::SameLine();
    ImGui::Checkbox("Chords", &s_p.do_chords);
    tip("Borrow chord charts from matching sections and repeated progressions");

    if (s_have_prop) {
        ImGui::TextWrapped("%s", s_prop.status);
    } else {
        ImGui::TextDisabled(beatmap->count >= 4
            ? "Analyze to propose beats, sections and chords for the unmapped parts."
            : "Map a verse or a chorus first; the tool repeats what you have.");
        return;
    }

    s_list_narrowed = editor->has_region;
    if (s_list_narrowed) {
        s_list_r0 = editor->region_start < editor->region_end ? editor->region_start : editor->region_end;
        s_list_r1 = editor->region_start < editor->region_end ? editor->region_end   : editor->region_start;
    }
    count_listed();
    ImGui::Checkbox("Ghosts", &s_show_ghosts);
    ImGui::SameLine();
    if (s_list_narrowed)
        ImGui::TextDisabled("%d of %d in region, %d ticked", s_n_listed, (int)s_prop.cands.size(), s_n_sel);
    else
        ImGui::TextDisabled("%d candidates, %d ticked", s_n_listed, s_n_sel);

    static const char* KIND_HDR[CAND_KIND_COUNT] = { "Beats", "Sections", "Chords" };
    static const ImU32 KIND_COL[CAND_KIND_COUNT] = {
        IM_COL32(255, 210, 110, 255), IM_COL32(150, 200, 255, 255), IM_COL32(200, 160, 255, 255),
    };

    if (ImGui::BeginChild("##cand_list", ImVec2(0, 0), true)) {
        int last_kind = -1;
        for (int i = 0; i < (int)s_prop.cands.size(); i++) {
            if (!ui_complete_cand_listed(i)) continue;
            CompleteCand& c = s_prop.cands[i];
            if ((int)c.kind != last_kind) {
                if (last_kind >= 0) ImGui::Spacing();
                ImGui::TextColored(ImColor(KIND_COL[c.kind]), "%s", KIND_HDR[c.kind]);
                last_kind = c.kind;
            }
            ImGui::PushID(i);
            ImGui::Checkbox("##sel", &c.selected);
            bool hov = ImGui::IsItemHovered();
            ImGui::SameLine();
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%.2f", c.score);
            ImGui::TextColored(c.score >= 0.75f ? ImVec4(0.55f, 0.95f, 0.55f, 1.0f)
                             : c.score >= 0.5f  ? ImVec4(0.95f, 0.85f, 0.45f, 1.0f)
                                                : ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", lbl);
            hov |= ImGui::IsItemHovered();
            ImGui::SameLine();
            // Dismiss button at the right edge; the description takes the rest
            float x_btn = ImGui::GetFrameHeight();
            float desc_w = ImGui::GetContentRegionAvail().x - x_btn - ImGui::GetStyle().ItemSpacing.x;
            if (desc_w < 40.0f) desc_w = 40.0f;
            ImGui::PushStyleColor(ImGuiCol_Text, c.selected ? IM_COL32(225, 225, 240, 255)
                                                            : IM_COL32(150, 150, 170, 255));
            if (ImGui::Selectable(c.desc, false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(desc_w, 0))) {
                // Click: bring it into view.  Double-click: toggle.
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) c.selected = !c.selected;
                double span = editor->view_end - editor->view_start;
                double mid  = 0.5 * (c.t0 + c.t1);
                if (c.t0 < editor->view_start || c.t1 > editor->view_end) {
                    double need = (c.t1 - c.t0) * 1.3;
                    if (need > span) span = need;
                    editor->view_start = mid - 0.5 * span;
                    editor->view_end   = mid + 0.5 * span;
                    editor_clamp_view(editor);
                }
            }
            ImGui::PopStyleColor();
            hov |= ImGui::IsItemHovered();
            if (hov) {
                s_hover = i;
                ImGui::SetTooltip("%s\nsource: %s\nclick: scroll into view, double-click: toggle",
                                  c.desc, c.source);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(160, 50, 50, 200));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200, 60, 60, 255));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(190, 120, 120, 220));
            bool dismiss = ImGui::Button("x", ImVec2(x_btn, 0));
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this suggestion");
            ImGui::PopID();
            if (dismiss) {
                s_prop.cands.erase(s_prop.cands.begin() + i);
                s_hover = -1;
                i--;            // the next candidate now sits at this index
                continue;
            }
        }
        if (s_n_listed == 0)
            ImGui::TextDisabled(s_list_narrowed ? "(no candidates intersect the region)"
                                                : "(nothing to propose)");
    }
    ImGui::EndChild();

}

void ui_complete_actions(ToolCtx& c)
{
    EditorState* editor = c.editor;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    bool loaded = c.audio->loaded && audio_pcm_data(c.audio, nullptr, nullptr, nullptr);

    // Row 1: Analyze
    if (!loaded) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.35f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.45f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
    if (ImGui::Button(editor->has_region ? "Analyze region" : "Analyze track", ImVec2(avail_w, 0)))
        run_analysis(editor, c.audio, c.beatmap, c.sectionmap, c.chordmap);
    ImGui::PopStyleColor(3);
    if (!loaded) ImGui::EndDisabled();
    tip(editor->has_region
        ? "Fill only gaps inside the selected region; list only matches that fit it"
        : "Fill every unmapped stretch of the track.  Select a region first to restrict it.");

    bool have = s_have_prop;
    if (!have) ImGui::BeginDisabled();
    count_listed();

    // Row 2: Select all | Deselect all | Discard
    float w3 = (avail_w - 2.0f * sp) / 3.0f;
    if (ImGui::Button("Select all", ImVec2(w3, 0)))
        for (int i = 0; i < (int)s_prop.cands.size(); i++)
            if (ui_complete_cand_listed(i)) s_prop.cands[i].selected = true;
    ImGui::SameLine();
    if (ImGui::Button("Deselect all", ImVec2(w3, 0)))
        for (int i = 0; i < (int)s_prop.cands.size(); i++)
            if (ui_complete_cand_listed(i)) s_prop.cands[i].selected = false;
    tip("Then tick them one at a time");
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(w3, 0))) ui_complete_reset();
    tip("Drop every proposal");

    // Row 3: second-stage smoothing
    if (s_n_beat_sel == 0) ImGui::BeginDisabled();
    char sb[48];
    snprintf(sb, sizeof(sb), "Smooth %d selected segment%s", s_n_beat_sel, s_n_beat_sel == 1 ? "" : "s");
    if (ImGui::Button(sb, ImVec2(avail_w, 0))) {
        std::vector<char> saved(s_prop.cands.size());
        for (int i = 0; i < (int)s_prop.cands.size(); i++) {
            saved[i] = s_prop.cands[i].selected;
            if (!ui_complete_cand_listed(i)) s_prop.cands[i].selected = false;
        }
        complete_smooth_segments(&s_prop, c.beatmap, s_p.smooth2, true);
        for (int i = 0; i < (int)s_prop.cands.size(); i++) s_prop.cands[i].selected = saved[i];
    }
    if (s_n_beat_sel == 0) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Second-stage smoothing: each ticked beat segment on its own,\n"
                          "pinned to the beats either side, guided by the fill's onsets.\n"
                          "Repeatable; tune it under Beat settings.");

    // Row 4: Accept
    bool can_accept = s_n_sel > 0;
    if (!can_accept) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f, 0.45f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.58f, 0.29f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.68f, 0.35f, 1.0f));
    char btn[48];
    snprintf(btn, sizeof(btn), "Accept %d selected", s_n_sel);
    if (ImGui::Button(btn, ImVec2(avail_w, 0)))
        accept_selected(c.beatmap, c.sectionmap, c.lyricmap, c.miscmap, c.chordmap, c.undo);
    ImGui::PopStyleColor(3);
    if (!can_accept) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(can_accept
            ? "Insert the ticked candidates (one Ctrl+Z undoes them all).\n"
              "Re-analyze afterwards: sections and chords are inferred from beats in the map."
            : "Tick candidates to accept");
    if (!have) ImGui::EndDisabled();
}
