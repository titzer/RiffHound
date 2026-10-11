#include "ui_lyric_find.h"
#include "ui_lyric_auto.h"
#include "lyrics_find.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static bool s_open_dialog   = false;   // open the results dialog next frame
static bool s_fresh         = false;   // results of a search the user asked for, not yet acted on
static bool s_want_search   = false;   // --find-lyrics
static int  s_sel           = 0;       // highlighted result in the dialog
static int  s_preview_for   = -1;
static std::vector<std::string> s_preview;
static std::string s_used_id;          // source_id the lyrics came from (marked in the dialog)
// The query: what the user typed for this track (remembered per track by
// lyrics_find_save_query; empty = let the script read tags / file name),
// and the dialog's edit fields, filled from what the last search used.
static std::string s_user_title, s_user_artist;
static char s_edit_title[128], s_edit_artist[128];

static void fill_edit_fields() {
    snprintf(s_edit_title,  sizeof(s_edit_title),  "%s", lyrics_find_query_title().c_str());
    snprintf(s_edit_artist, sizeof(s_edit_artist), "%s", lyrics_find_query_artist().c_str());
}

// An online search with the track's query (typed, or else from the file).
static void search(const AudioState* audio) {
    lyrics_find_start(audio->filename, false, s_user_title.c_str(), s_user_artist.c_str());
}

int lyric_add_unplaced(LyricMap* lm, double dur, const std::vector<std::string>& lines) {
    double base = lyricmap_next_slot(lm, dur);
    int added = 0;
    for (const std::string& l : lines) {
        // Trim, and keep within the lyric's text buffer
        size_t a = 0, b = l.size();
        while (a < b && (l[a] == ' ' || l[a] == '\t')) a++;
        while (b > a && (l[b - 1] == ' ' || l[b - 1] == '\t')) b--;
        if (b == a) continue;
        char buf[128] = {};
        size_t n = b - a < sizeof(buf) - 1 ? b - a : sizeof(buf) - 1;
        memcpy(buf, l.data() + a, n);
        double t = base + added * LYRIC_SLOT;
        lyricmap_add(lm, t, t + 1e-6, buf);
        added++;
    }
    return added;
}

static void load_hints(const LyricsCand& c) {
    std::vector<LyricTimedLine> timed;
    if (c.kind == "synced" && lyrics_read_timed(c.timed_path, &timed))
        ui_lyric_auto_set_hints(timed);
    else
        ui_lyric_auto_set_hints({});
    printf("[lyrics] hints: %d timed lines from %s\n", (int)timed.size(), c.source_id.c_str());
}

void ui_lyric_find_track_loaded(const AudioState* audio) {
    lyrics_find_reset();
    s_open_dialog = s_fresh = false;
    s_preview_for = -1;
    s_used_id.clear();
    s_edit_title[0] = s_edit_artist[0] = 0;
    lyrics_find_load_query(audio->filename, &s_user_title, &s_user_artist);
    // A cache check, unless a search was asked for at launch; with the
    // title / artist typed for this track before, if any
    if (audio->loaded && lyrics_find_available())
        lyrics_find_start(audio->filename, !s_want_search,
                          s_user_title.c_str(), s_user_artist.c_str());
    s_want_search = false;
}

void ui_lyric_find_request() { s_want_search = true; }

void ui_lyric_find_frame(const AudioState* audio) {
    (void)audio;
    if (!lyrics_find_poll()) return;
    fill_edit_fields();
    const std::vector<LyricsCand>& r = lyrics_find_results();
    if (lyrics_find_was_cached_only()) {
        // Searched before: bring back the picked result's timings.
        std::string id = lyrics_find_remembered_choice();
        for (const LyricsCand& c : r)
            if (c.source_id == id) { s_used_id = id; load_hints(c); break; }
        return;
    }
    s_fresh = true;
    s_sel = 0;
    s_preview_for = -1;
}

// Take result `c`: its lines become unplaced lyrics (replacing the unplaced
// ones when asked), its timings auto-insert hints.
static void use_result(const LyricsCand& c, bool replace, AudioState* audio,
                       BeatMap* beatmap, UndoStack* undo, LyricMap* lm) {
    std::vector<std::string> lines;
    if (!lyrics_read_lines(c.txt_path, &lines) || lines.empty()) return;
    double dur = audio->duration > 0.0 ? audio->duration : 0.0;
    undo_push(undo, beatmap, lm);
    if (replace)
        for (int i = lm->count - 1; i >= 0; i--)
            if (lm->lyrics[i].t_start >= dur - 1e-9) lyricmap_remove(lm, i);
    int n = lyric_add_unplaced(lm, dur, lines);
    if (n == 0) { undo_drop_last(undo); return; }
    lyrics_find_remember_choice(c);
    s_used_id = c.source_id;
    load_hints(c);
    // Synced lyrics are suggestions straight away: show them
    if (ui_lyric_auto_has_hints()) ui_lyric_auto_set_active(true);
    printf("[lyrics] added %d lines from %s (%s - %s)%s\n", n, c.source_id.c_str(),
           c.artist.c_str(), c.title.c_str(), replace ? ", replacing the unplaced" : "");
}

static void fmt_dur(double d, char* out, int n) {
    if (d < 0) { snprintf(out, n, "?"); return; }
    int m = (int)(d / 60.0);
    snprintf(out, n, "%d:%02d", m, (int)(d - 60.0 * m));
}

static void results_dialog(AudioState* audio, BeatMap* beatmap, UndoStack* undo, LyricMap* lm) {
    if (s_open_dialog) { ImGui::OpenPopup("Lyrics found"); s_open_dialog = false; }
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x * 0.55f < 760 ? 760 : vp.x * 0.55f, vp.y * 0.7f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("Lyrics found", &open)) return;
    const std::vector<LyricsCand>& r = lyrics_find_results();
    bool busy = lyrics_find_status() == LF_RUNNING;

    // Search row: the title and artist the results are for, editable --
    // the file's tags or name can be wrong or missing.
    {
        float sp = ImGui::GetStyle().ItemSpacing.x;
        float bw = ImGui::CalcTextSize("Search").x + ImGui::GetStyle().FramePadding.x * 2 + 8.0f;
        float avail = ImGui::GetContentRegionAvail().x - bw - sp * 2;
        bool go = false;
        if (busy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(avail * 0.58f);
        go |= ImGui::InputTextWithHint("##lftitle", "Title", s_edit_title, sizeof(s_edit_title),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(avail * 0.42f);
        go |= ImGui::InputTextWithHint("##lfartist", "Artist (optional)", s_edit_artist,
                                       sizeof(s_edit_artist), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        go |= ImGui::Button("Search", ImVec2(bw, 0));
        if (busy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Search again for this title and artist (Enter in a field does too).\n"
                              "Remembered for this track.");
        if (go && s_edit_title[0]) {
            s_user_title  = s_edit_title;
            s_user_artist = s_edit_artist;
            lyrics_find_save_query(audio->filename, s_user_title, s_user_artist);
            search(audio);
            s_sel = 0;
            s_preview_for = -1;
        }
    }
    ImGui::Separator();
    if (busy) {
        ImGui::TextDisabled("Searching\xe2\x80\xa6 %s", lyrics_find_message());
        ImGui::Spacing();
        if (ImGui::Button("Close") || !open) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (r.empty()) {
        const std::string& qt = lyrics_find_query_title();
        const std::string& qa = lyrics_find_query_artist();
        if (lyrics_find_status() == LF_FAILED)
            ImGui::TextWrapped("%s", lyrics_find_message());
        else if (!qa.empty())
            ImGui::TextWrapped("No lyrics found for \"%s\" by %s.", qt.c_str(), qa.c_str());
        else
            ImGui::TextWrapped("No lyrics found for \"%s\".", qt.c_str());
        ImGui::TextDisabled("Correct the title (and artist) above and search again.");
        ImGui::Spacing();
        if (ImGui::Button("Close") || !open) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (s_sel >= (int)r.size()) s_sel = 0;

    ImGui::TextDisabled("Pick the right song; synced lyrics also help auto-insert place the lines.");
    float list_h = ImGui::GetContentRegionAvail().y * 0.42f;
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##lfres", 6, tf, ImVec2(0, list_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Title",  ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Timing", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Lines",  ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)r.size(); i++) {
            const LyricsCand& c = r[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            char lbl[300];
            snprintf(lbl, sizeof(lbl), "%s%s", c.title.c_str(),
                     c.source_id == s_used_id ? "  (in use)" : "");
            if (ImGui::Selectable(lbl, i == s_sel,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick))
                s_sel = i;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.artist.c_str());
            ImGui::TableNextColumn();
            if (c.kind == "synced") ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1), "synced");
            else                    ImGui::TextDisabled("%s", c.kind.c_str());
            ImGui::TableNextColumn();
            char d[16]; fmt_dur(c.duration, d, sizeof(d));
            double dd = c.duration >= 0 ? c.duration - audio->duration : 0.0;
            if (c.duration >= 0 && fabs(dd) >= 1.5)
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1), "%s (%+.0fs)", d, dd);
            else
                ImGui::TextUnformatted(d);
            if (ImGui::IsItemHovered() && c.duration >= 0)
                ImGui::SetTooltip("This track: %.1f s; result: %.1f s", audio->duration, c.duration);
            ImGui::TableNextColumn(); ImGui::Text("%d", c.n_lines);
            ImGui::TableNextColumn();
            std::string src = c.source_id.substr(0, c.source_id.find(':'));
            ImGui::TextDisabled("%s", src.c_str());
        }
        ImGui::EndTable();
    }

    // Preview of the highlighted result
    if (s_preview_for != s_sel) {
        lyrics_read_lines(r[s_sel].txt_path, &s_preview);
        s_preview_for = s_sel;
    }
    const LyricsCand& cur = r[s_sel];
    float btn_h = ImGui::GetFrameHeightWithSpacing() + 6.0f;
    if (ImGui::BeginChild("##lfprev", ImVec2(0, ImGui::GetContentRegionAvail().y - btn_h), true)) {
        if (s_preview.empty()) ImGui::TextDisabled("(no text)");
        for (const std::string& l : s_preview) ImGui::TextUnformatted(l.c_str());
    }
    ImGui::EndChild();

    int n_unplaced = 0;
    double dur = audio->duration;
    for (int i = 0; i < lm->count; i++) if (lm->lyrics[i].t_start >= dur - 1e-9) n_unplaced++;
    bool usable = !s_preview.empty();
    if (!usable) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f, 0.45f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.58f, 0.29f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.68f, 0.35f, 1.0f));
    bool add = ImGui::Button(n_unplaced ? "Add as unplaced" : "Use these lyrics");
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add the %d lines as unplaced lyrics%s", (int)s_preview.size(),
                          cur.kind == "synced" ? "; their timings guide auto-insert" : "");
    bool repl = false;
    if (n_unplaced) {
        ImGui::SameLine();
        repl = ImGui::Button("Replace unplaced");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drop the %d unplaced lyrics and add these instead (placed ones stay)",
                              n_unplaced);
    }
    if (!usable) ImGui::EndDisabled();
    if (cur.kind == "synced" && cur.source_id != s_used_id && n_unplaced) {
        ImGui::SameLine();
        if (ImGui::Button("Timings only")) {
            lyrics_find_remember_choice(cur);
            s_used_id = cur.source_id;
            load_hints(cur);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keep the lyrics as they are; use this result's timings\n"
                              "to guide auto-insert");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || !open) ImGui::CloseCurrentPopup();
    if (add || repl) {
        use_result(cur, repl, audio, beatmap, undo, lm);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ui_lyric_find_button(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                          UndoStack* undo, LyricMap* lm) {
    (void)editor;
    const std::vector<LyricsCand>& r = lyrics_find_results();
    LyricsFindStatus st = lyrics_find_status();
    bool searching = st == LF_RUNNING && !lyrics_find_was_cached_only();

    // A search the user asked for just finished: one result goes straight
    // in when there are no lyrics yet; otherwise show the choices.
    // (and when the dialog is up, it shows them there; no results opens it
    // too, saying what was searched for, so the title can be corrected)
    if (s_fresh) {
        s_fresh = false;
        bool in_dialog = ImGui::IsPopupOpen("Lyrics found");
        if (r.size() == 1 && lm->count == 0 && !in_dialog)
            use_result(r[0], false, audio, beatmap, undo, lm);
        else
            s_open_dialog = true;
    }

    if (searching) {
        ImGui::BeginDisabled();
        ImGui::SmallButton("Searching\xe2\x80\xa6");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", lyrics_find_message());
    } else if (!r.empty()) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "Results (%d)", (int)r.size());
        if (ImGui::SmallButton(lbl)) s_open_dialog = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lyrics found online for this track: pick one, or\n"
                              "search with another title (right-click: search again)");
        if (ImGui::BeginPopupContextItem("##lfctx")) {
            if (ImGui::MenuItem("Search again")) search(audio);
            ImGui::EndPopup();
        }
    } else {
        bool can = audio->loaded && lyrics_find_available() && st != LF_RUNNING;
        if (!can) ImGui::BeginDisabled();
        // After a search that found nothing, Find opens the dialog to fix
        // the title instead of repeating the same search.
        if (ImGui::SmallButton("Find")) {
            if (st == LF_DONE || st == LF_FAILED) {
                if (!s_edit_title[0]) fill_edit_fields();
                s_open_dialog = true;
            } else {
                search(audio);
            }
        }
        if (!can) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!lyrics_find_available())
                ImGui::SetTooltip("scripts/lyrics_find.py not found");
            else if (st == LF_DONE)
                ImGui::SetTooltip("No lyrics found for this track.  Click to search\n"
                                  "with a different title or artist.");
            else if (st == LF_FAILED)
                ImGui::SetTooltip("%s\nClick to search again.", lyrics_find_message());
            else
                ImGui::SetTooltip("Look this track's lyrics up online (LRCLIB, NetEase),\n"
                                  "by its tags or file name and length");
        }
    }
    results_dialog(audio, beatmap, undo, lm);
}
