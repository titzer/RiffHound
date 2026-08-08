#include "ui_annstrip.h"
#include "editor.h"
#include "beatmap.h"
#include "undo.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- geometry constants ---------------------------------------------------

const float ANN_PAD      = 4.0f;   // strip edge to the first row
const float ANN_ROW_H    = 20.0f;  // one annotation row
const float ANN_ROW_GAP  = 2.0f;
const float ANN_GRIP_H   = 4.0f;   // bottom band that resizes the lane -- the
                                   // padding itself, so it never covers a row
const float ANN_MIN_H    = ANN_PAD * 2 + ANN_ROW_H;
const float ANN_MAX_H    = ANN_PAD * 2 + ANN_ROW_H * 8 + ANN_ROW_GAP * 7;

int annstrip_rows(float height) {
    float room = height - ANN_PAD * 2 + ANN_ROW_GAP;
    int   n    = (int)(room / (ANN_ROW_H + ANN_ROW_GAP));
    return n < 1 ? 1 : n;
}

// --- one lane -------------------------------------------------------------

struct AnnStrip {
    MiscMap*    map;
    const char* label;
    PanelId     panel;
    float EditorState::* height;   // where this lane's height is stored

    // Colours: the lane is told apart by hue, not by reading its label.
    ImU32 fill, fill_sel, edge, edge_sel, text, hdr, preview, band;

    // Geometry, refreshed every frame by annstrip_place().
    bool  show;
    float x, y, w, h;

    // Row layout, one entry per annotation, recomputed every frame.
    int*  rows;
    int   rows_cap;
    int   rows_n;      // entries in rows[] that are valid this frame
    int   row_count;   // rows actually in use
    bool  crowded;     // something had to share a row it did not clear

    // Interaction.
    bool   in;             // a drag that started in this lane is live
    bool   creating;       // drag-to-create
    double create_t0, create_t1;
    bool   hdrag;          // edge resize of one annotation
    int    hdrag_idx, hdrag_end;
    double hdrag_t0, hdrag_t1;
    bool   rect_sel;
    float  rect_x0;
    bool   body_drag;      // translate the whole selection
    double body_grab, body_t0, body_delta;
    bool   grip;           // dragging the lane's own bottom edge
    float  grip_dy;        // cursor to bottom edge at grab

    int    focus;          // the entry inline editing and edge drags act on
    int    focus_sync;     // last value stamped into the map
    int    editing;        // entry being inline-edited, or -1
};

static AnnStrip s_ann[ANN_COUNT];
static int      s_active = ANN_MISC;   // lane the keyboard acts on

void annstrip_init(MiscMap* chords, MiscMap* misc) {
    AnnStrip& c = s_ann[ANN_CHORDS];
    memset(&c, 0, sizeof(c));
    c.map    = chords;
    c.label  = "Chords";
    c.panel  = PANEL_CHORDS;
    c.height = &EditorState::chord_strip_h;
    c.fill     = IM_COL32( 30,  80,  68, 150);
    c.fill_sel = IM_COL32( 45, 120, 100, 190);
    c.edge     = IM_COL32(  0,   0,   0, 100);
    c.edge_sel = IM_COL32(105, 215, 180, 235);
    c.text     = IM_COL32(205, 245, 230, 235);
    c.hdr      = IM_COL32( 90, 150, 135, 160);
    c.preview  = IM_COL32( 60, 170, 140,  60);
    c.band     = IM_COL32(110, 220, 190,  40);
    c.focus = c.focus_sync = c.editing = -1;
    c.hdrag_idx = -1;

    AnnStrip& m = s_ann[ANN_MISC];
    memset(&m, 0, sizeof(m));
    m.map    = misc;
    m.label  = "Misc";
    m.panel  = PANEL_MISC;
    m.height = &EditorState::misc_strip_h;
    m.fill     = IM_COL32( 65,  40,  95, 140);
    m.fill_sel = IM_COL32( 90,  55, 130, 180);
    m.edge     = IM_COL32(  0,   0,   0, 100);
    m.edge_sel = IM_COL32(160, 120, 210, 230);
    m.text     = IM_COL32(220, 210, 240, 230);
    m.hdr      = IM_COL32(120, 110, 150, 160);
    m.preview  = IM_COL32(130,  80, 180,  60);
    m.band     = IM_COL32(160, 140, 220,  40);
    m.focus = m.focus_sync = m.editing = -1;
    m.hdrag_idx = -1;
}

MiscMap* annstrip_map  (AnnStripId id) { return s_ann[id].map; }
PanelId  annstrip_panel(AnnStripId id) { return s_ann[id].panel; }

float annstrip_height(const EditorState* e, AnnStripId id) {
    float h = e->*(s_ann[id].height);
    if (h < ANN_MIN_H) h = ANN_MIN_H;
    if (h > ANN_MAX_H) h = ANN_MAX_H;
    return h;
}

// --- helpers --------------------------------------------------------------

static float time_to_x(double t, const EditorState* e, float x0, float w) {
    double span = e->view_end - e->view_start;
    if (span <= 0.0) return x0;
    return x0 + (float)((t - e->view_start) / span * w);
}

static double x_to_time(float px, const AnnStrip& s, const EditorState* e) {
    double span = e->view_end - e->view_start;
    if (s.w <= 0.0f || span <= 0.0) return e->view_start;
    return e->view_start + (double)(px - s.x) / s.w * span;
}

// Snap t to the nearest beat within snap_px pixels of it.
static double snap_to_beat(double t, const BeatMap* bm, const EditorState* e,
                           float area_w, float snap_px = 12.0f) {
    if (!bm || bm->count == 0 || area_w <= 0) return t;
    double span = e->view_end - e->view_start;
    double tol  = (double)(snap_px / area_w) * span;
    int lo = 0, hi = bm->count;
    while (lo < hi) { int m = (lo+hi)/2; if (bm->beats[m].time < t) lo=m+1; else hi=m; }
    double best = t, bd = tol + 1e-9;
    if (lo > 0) {
        double d = t - bm->beats[lo-1].time;
        if (d < bd) { bd = d; best = bm->beats[lo-1].time; }
    }
    if (lo < bm->count) {
        double d = bm->beats[lo].time - t;
        if (d < bd) best = bm->beats[lo].time;
    }
    return best;
}

// A keyboard paste has no pixel tolerance to snap by, so it snaps by musical
// distance instead; an anchor deliberately placed mid-beat is left alone.
static double snap_anchor(double t, const BeatMap* bm, double frac = 0.35) {
    if (!bm || bm->count < 2) return t;
    double b  = beatmap_beat_pos(bm, t);
    double nb = floor(b + 0.5);
    if (fabs(b - nb) > frac) return t;
    return beatmap_time_at(bm, nb);
}

// Only this lane's layer is snapshotted, so undoing a chord edit cannot
// resurrect misc annotations changed since.
static void push_undo(const AnnStrip& s, UndoStack* undo) {
    if (&s == &s_ann[ANN_CHORDS]) undo_push(undo, nullptr, nullptr, nullptr, nullptr, s.map);
    else                          undo_push(undo, nullptr, nullptr, nullptr, s.map);
}

static float row_y(const AnnStrip& s, int row) {
    return s.y + ANN_PAD + row * (ANN_ROW_H + ANN_ROW_GAP);
}

// The delta the drawing uses for an entry: a body drag is virtual until
// release, so the moving group is drawn where it is being dragged to.
static double draw_delta(const AnnStrip& s, const MiscAnnotation& ma) {
    return (s.body_drag && ma.selected) ? s.body_delta : 0.0;
}

// undo_pop() clears selected_idx behind our back; adopt an outside change
// rather than stamping a stale focus index back over it.
static void sync_focus(AnnStrip& s) {
    if (s.map->selected_idx != s.focus_sync) s.focus = s.map->selected_idx;
    if (s.focus >= s.map->count) s.focus = -1;
    s.map->selected_idx = s.focus_sync = s.focus;
}

// --- row layout -----------------------------------------------------------
// Greedy first fit over the rows the lane's height allows: an annotation goes
// on the topmost row it clears, so a lane holding one kind of thing stays one
// row deep and only actual overlap costs vertical space.  When everything is
// occupied it joins the row that frees up soonest, which keeps the damage local
// rather than hiding the entry entirely.

#define ANN_MAX_ROWS 8

// The row an entry was laid out on.  Anything the layout did not reach --
// added since, or lost to a failed allocation -- falls back to the top row
// rather than reading past the array.
static int row_of(const AnnStrip& s, int i) {
    return (s.rows && i < s.rows_n) ? s.rows[i] : 0;
}

static void layout_rows(AnnStrip& s, const EditorState* e) {
    s.row_count = 0;
    s.rows_n    = 0;
    s.crowded   = false;
    MiscMap* mm = s.map;
    if (!mm || mm->count == 0) return;

    if (s.rows_cap < mm->count) {
        int cap = mm->count < 64 ? 64 : mm->count;
        int* p  = (int*)realloc(s.rows, cap * sizeof(int));
        if (!p) return;
        s.rows     = p;
        s.rows_cap = cap;
    }

    int nrows = annstrip_rows(s.h);
    if (nrows > ANN_MAX_ROWS) nrows = ANN_MAX_ROWS;
    const float gap = 3.0f;
    float right[ANN_MAX_ROWS];
    for (int r = 0; r < nrows; r++) right[r] = -1e9f;

    for (int i = 0; i < mm->count; i++) {
        double d  = draw_delta(s, mm->entries[i]);
        float  x0 = time_to_x(mm->entries[i].t_start + d, e, s.x, s.w);
        float  x1 = time_to_x(mm->entries[i].t_end   + d, e, s.x, s.w);
        if (x1 < x0 + 2.0f) x1 = x0 + 2.0f;

        int best = -1;
        for (int r = 0; r < nrows; r++)
            if (x0 >= right[r] + gap) { best = r; break; }
        if (best < 0) {
            // Every row is still busy.  Join the one that frees up soonest and
            // remember that the lane is too short for what is in it.
            best = 0;
            for (int r = 1; r < nrows; r++) if (right[r] < right[best]) best = r;
            s.crowded = true;
        }
        right[best] = x1;
        s.rows[i]   = best;
        if (best + 1 > s.row_count) s.row_count = best + 1;
    }
    s.rows_n = mm->count;
}

void annstrip_place(AnnStripId id, const EditorState* e, bool show,
                    float x, float y, float w, float h) {
    AnnStrip& s = s_ann[id];
    s.show = show;
    s.x = x; s.y = y; s.w = w; s.h = h;
    sync_focus(s);
    layout_rows(s, e);
}

// The entry under (mx, my), and which part of it: 0 = start handle,
// 1 = end handle, 2 = body.  -1 when the point is over empty lane.
static int hit_test(const AnnStrip& s, const EditorState* e,
                    float mx, float my, int* part) {
    const float HANDLE_PX = 6.0f;
    MiscMap* mm = s.map;
    for (int i = 0; i < mm->count; i++) {
        float ry = row_y(s, row_of(s, i));
        if (my < ry || my >= ry + ANN_ROW_H) continue;
        double d  = draw_delta(s, mm->entries[i]);
        float  x0 = time_to_x(mm->entries[i].t_start + d, e, s.x, s.w);
        float  x1 = time_to_x(mm->entries[i].t_end   + d, e, s.x, s.w);
        if (fabsf(mx - x0) <= HANDLE_PX) { *part = 0; return i; }
        if (fabsf(mx - x1) <= HANDLE_PX) { *part = 1; return i; }
        if (mx > x0 && mx < x1)          { *part = 2; return i; }
    }
    return -1;
}

void annstrip_defocus(AnnStripId id) {
    AnnStrip& s = s_ann[id];
    miscmap_clear_selection(s.map);
    s.focus   = -1;
    s.editing = -1;
}

bool annstrip_click(AnnStripId id, const AnnStripCtx& c, float mx, float my) {
    AnnStrip& s = s_ann[id];
    if (!s.show || mx < s.x || my < s.y || my >= s.y + s.h) return false;

    ImGuiIO& io = ImGui::GetIO();
    s.in     = true;
    s_active = id;

    // The bottom band resizes the lane rather than editing what is in it.
    if (my >= s.y + s.h - ANN_GRIP_H) {
        s.grip    = true;
        s.grip_dy = (s.y + s.h) - my;
        s.creating = s.hdrag = s.rect_sel = s.body_drag = false;
        return true;
    }

    double t_raw  = x_to_time(mx, s, c.editor);
    double t_snap = snap_to_beat(t_raw, c.beatmap, c.editor, s.w);

    int part = 2;
    int hit  = hit_test(s, c.editor, mx, my, &part);

    if (hit >= 0) {
        s.creating = false;
        if (io.KeyShift) {
            // Shift+click toggles group membership -- no edge drag, no inline
            // edit, so a group can be assembled click by click.
            bool on = !s.map->entries[hit].selected;
            s.map->entries[hit].selected = on;
            s.focus     = on ? hit : -1;
            s.editing   = -1;
            s.hdrag     = false;
            s.body_drag = false;
        } else {
            // Clicking a member keeps the group (so it can be copied right
            // after); clicking a non-member replaces it.
            if (!s.map->entries[hit].selected) {
                miscmap_clear_selection(s.map);
                s.map->entries[hit].selected = true;
            }
            s.focus = hit;
            if (part < 2) {
                s.hdrag     = true;
                s.hdrag_idx = hit;
                s.hdrag_end = part;
                s.hdrag_t0  = s.map->entries[hit].t_start;
                s.hdrag_t1  = s.map->entries[hit].t_end;
                s.body_drag = false;
                push_undo(s, c.undo);
            } else {
                // Body: arm a translate drag of the whole selection.  A click
                // that never moves is a no-op on release.
                s.hdrag      = false;
                s.body_drag  = true;
                s.body_t0    = s.map->entries[hit].t_start;
                s.body_grab  = t_raw - s.body_t0;
                s.body_delta = 0.0;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    s.editing   = hit;
                    s.body_drag = false;
                }
            }
        }
    } else if (io.KeyShift) {
        // Shift+drag over empty lane rubber-bands a group instead of creating.
        s.rect_sel  = true;
        s.rect_x0   = mx;
        s.focus     = -1;
        s.editing   = -1;
        s.creating  = false;
        s.hdrag     = false;
        s.body_drag = false;
    } else {
        miscmap_clear_selection(s.map);
        s.focus     = -1;
        s.editing   = -1;
        s.creating  = true;
        s.create_t0 = t_snap;
        s.create_t1 = t_snap;
        s.hdrag     = false;
        s.body_drag = false;
    }
    return true;
}

void annstrip_drag(AnnStripId id, const AnnStripCtx& c) {
    AnnStrip&    s  = s_ann[id];
    ImGuiIO&     io = ImGui::GetIO();
    EditorState* e  = c.editor;
    bool dragging   = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    bool released   = !ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Lane resize: the bottom edge follows the cursor, within what the lane
    // can be.  Height is in pixels rather than a row count so the drag is
    // continuous and the rows appear as they fit.
    if (s.grip) {
        if (dragging) {
            float h = io.MousePos.y + s.grip_dy - s.y;
            if (h < ANN_MIN_H) h = ANN_MIN_H;
            if (h > ANN_MAX_H) h = ANN_MAX_H;
            e->*(s.height) = h;
        }
        if (released) s.grip = false;
    }

    if (s.creating && s.in && dragging)
        s.create_t1 = snap_to_beat(x_to_time(io.MousePos.x, s, e), c.beatmap, e, s.w);

    // Body drag: the map itself is untouched until release -- moving in place
    // would re-sort the array under the indices this drag is holding.
    if (s.body_drag && s.in && dragging) {
        double d = snap_to_beat(x_to_time(io.MousePos.x, s, e) - s.body_grab,
                                c.beatmap, e, s.w) - s.body_t0;
        double lo = 0.0, hi = 0.0;
        bool   any = false;
        for (int i = 0; i < s.map->count; i++) {
            if (!s.map->entries[i].selected) continue;
            if (!any || s.map->entries[i].t_start < lo) lo = s.map->entries[i].t_start;
            if (!any || s.map->entries[i].t_end   > hi) hi = s.map->entries[i].t_end;
            any = true;
        }
        if (any) {
            if (lo + d < 0.0)         d = -lo;
            if (hi + d > e->duration) d = e->duration - hi;
        }
        s.body_delta = d;
    }

    if (s.body_drag && released) {
        if (fabs(s.body_delta) > 1e-6 && miscmap_selected_count(s.map) > 0) {
            push_undo(s, c.undo);
            int focus = s.focus;
            miscmap_move_selection(s.map, s.body_delta, &focus);
            s.focus = focus;
        }
        s.body_drag  = false;
        s.body_delta = 0.0;
    }

    if (s.hdrag && dragging && s.hdrag_idx >= 0 && s.hdrag_idx < s.map->count) {
        double t = snap_to_beat(x_to_time(io.MousePos.x, s, e), c.beatmap, e, s.w);
        MiscAnnotation& ma = s.map->entries[s.hdrag_idx];
        if (s.hdrag_end == 0)
            ma.t_start = (t < ma.t_end - 0.001) ? t : ma.t_end - 0.001;
        else
            ma.t_end   = (t > ma.t_start + 0.001) ? t : ma.t_start + 0.001;
        s.map->dirty = true;
    }

    if (s.creating && released) {
        double t0 = s.create_t0 < s.create_t1 ? s.create_t0 : s.create_t1;
        double t1 = s.create_t0 < s.create_t1 ? s.create_t1 : s.create_t0;
        if (t1 - t0 > 0.05) {
            push_undo(s, c.undo);
            int idx = miscmap_add(s.map, t0, t1, "");
            if (idx >= 0) {
                s.map->entries[idx].selected = true;
                s.focus   = idx;
                s.editing = idx;   // straight into typing: an empty one is useless
            } else {
                undo_drop_last(c.undo);
            }
        }
        s.creating = false;
    }

    if (s.hdrag && released) {
        if (s.hdrag_idx >= 0 && s.hdrag_idx < s.map->count) {
            const MiscAnnotation& ma = s.map->entries[s.hdrag_idx];
            if (fabs(ma.t_start - s.hdrag_t0) < 1e-9 &&
                fabs(ma.t_end   - s.hdrag_t1) < 1e-9)
                undo_drop_last(c.undo);
        }
        s.hdrag     = false;
        s.hdrag_idx = -1;
    }

    // Rubber-band release: everything overlapping the swept x range joins the
    // selection (shift is held, so it adds rather than replaces).
    if (s.rect_sel && released) {
        float x0 = s.rect_x0 < io.MousePos.x ? s.rect_x0 : io.MousePos.x;
        float x1 = s.rect_x0 < io.MousePos.x ? io.MousePos.x : s.rect_x0;
        for (int i = 0; i < s.map->count; i++) {
            float a = time_to_x(s.map->entries[i].t_start, e, s.x, s.w);
            float b = time_to_x(s.map->entries[i].t_end,   e, s.x, s.w);
            if (b >= x0 && a <= x1) s.map->entries[i].selected = true;
        }
        s.rect_sel = false;
    }

    s.map->selected_idx = s.focus_sync = s.focus;
}

void annstrip_release_all() {
    for (int i = 0; i < ANN_COUNT; i++) s_ann[i].in = false;
}

bool annstrip_resizing() {
    for (int i = 0; i < ANN_COUNT; i++) if (s_ann[i].grip) return true;
    return false;
}

// --- keyboard -------------------------------------------------------------

static void delete_selection(AnnStrip& s, UndoStack* undo) {
    if (s.focus >= 0 && s.focus < s.map->count)
        s.map->entries[s.focus].selected = true;
    if (miscmap_selected_count(s.map) == 0) return;
    push_undo(s, undo);
    for (int i = s.map->count - 1; i >= 0; i--)
        if (s.map->entries[i].selected) miscmap_remove(s.map, i);
    s.focus   = -1;
    s.editing = -1;
}

void annstrip_keys(const AnnStripCtx& c) {
    AnnStrip& s = s_ann[s_active];
    if (!s.show || ImGui::IsAnyItemActive() || s.editing >= 0) return;

    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        delete_selection(s, c.undo);
        s.map->selected_idx = s.focus_sync = s.focus;
        return;
    }
    if (!(io.KeyCtrl || io.KeySuper)) return;

    // Paste lands the group's first annotation at the playhead, snapped to the
    // nearest beat, with the rest at their original beat offsets.  Holding the
    // playhead still and pasting again tiles the next copy after the last, so
    // laying a copied bar of chords across eight bars is eight keystrokes.
    bool copy  = ImGui::IsKeyPressed(ImGuiKey_C);
    bool cut   = ImGui::IsKeyPressed(ImGuiKey_X);
    bool paste = ImGui::IsKeyPressed(ImGuiKey_V);

    if (copy || cut) {
        if (s.focus >= 0 && s.focus < s.map->count)
            s.map->entries[s.focus].selected = true;
        if (miscmap_selected_count(s.map) > 0) {
            miscmap_copy_selection(s.map, c.beatmap);
            if (cut) delete_selection(s, c.undo);
        }
    } else if (paste && miscmap_clipboard_count(s.map) > 0) {
        double anchor = snap_anchor(c.playhead, c.beatmap);
        push_undo(s, c.undo);
        if (miscmap_paste_chained(s.map, c.beatmap, anchor) > 0) {
            s.focus   = -1;   // the pasted group is selected, no focus
            s.editing = -1;
        } else {
            undo_drop_last(c.undo);
        }
    }
    s.map->selected_idx = s.focus_sync = s.focus;
}

// --- drawing --------------------------------------------------------------

void annstrip_draw(AnnStripId id, const AnnStripCtx& c, ImDrawList* dl,
                   float label_x) {
    AnnStrip& s = s_ann[id];
    if (!s.show) return;
    EditorState* e = c.editor;

    dl->AddRectFilled(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h),
                      IM_COL32(20, 18, 28, 255));
    dl->AddRect(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h),
                IM_COL32(60, 55, 80, 255));

    // The header carries the selection and clipboard counts -- otherwise a
    // group cut is invisible until something is pasted.
    char hdr[80];
    {
        int nsel  = miscmap_selected_count(s.map);
        int nclip = miscmap_clipboard_count(s.map);
        if (nsel > 1 && nclip > 0)
            snprintf(hdr, sizeof(hdr), "%s  %d sel  %d copied", s.label, nsel, nclip);
        else if (nsel > 1)  snprintf(hdr, sizeof(hdr), "%s  %d sel", s.label, nsel);
        else if (nclip > 0) snprintf(hdr, sizeof(hdr), "%s  %d copied", s.label, nclip);
        else                snprintf(hdr, sizeof(hdr), "%s", s.label);
    }
    dl->AddText(ImVec2(label_x, s.y + 2.0f), s.hdr, hdr);

    dl->PushClipRect(ImVec2(s.x, s.y), ImVec2(s.x + s.w, s.y + s.h), true);

    for (int i = 0; i < s.map->count; i++) {
        const MiscAnnotation& ma = s.map->entries[i];
        double d  = draw_delta(s, ma);
        float  x0 = time_to_x(ma.t_start + d, e, s.x, s.w);
        float  x1 = time_to_x(ma.t_end   + d, e, s.x, s.w);
        if (x1 <= s.x || x0 >= s.x + s.w) continue;

        float y0 = row_y(s, row_of(s, i));
        float y1 = y0 + ANN_ROW_H;
        if (y1 > s.y + s.h - 1.0f) continue;   // does not fit the lane as sized

        bool sel   = ma.selected || (i == s.focus);
        bool focus = (i == s.focus);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), sel ? s.fill_sel : s.fill);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), sel ? s.edge_sel : s.edge,
                    0.0f, 0, sel ? 2.0f : 1.0f);

        if (ma.text[0] && x1 - x0 > 8.0f) {
            float ty = y0 + (ANN_ROW_H - ImGui::GetTextLineHeight()) * 0.5f;
            ImVec2 ts = ImGui::CalcTextSize(ma.text);
            if (x0 + 4.0f + ts.x < x1 - 2.0f) {
                dl->AddText(ImVec2(x0 + 4.0f, ty), s.text, ma.text);
            } else {
                dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1 - 2.0f, y1), true);
                dl->AddText(ImVec2(x0 + 4.0f, ty), s.text, ma.text);
                dl->PopClipRect();
            }
        }
        // Resize handles for the focused annotation only -- an edge drag acts
        // on one entry, so the rest of the group must not offer them.
        if (focus) {
            float hy = (y0 + y1) * 0.5f;
            dl->AddRectFilled(ImVec2(x0 - 3.0f, hy - 7.0f),
                              ImVec2(x0 + 3.0f, hy + 7.0f), s.edge_sel);
            dl->AddRectFilled(ImVec2(x1 - 3.0f, hy - 7.0f),
                              ImVec2(x1 + 3.0f, hy + 7.0f), s.edge_sel);
        }
    }

    // More annotations overlap here than the lane is tall enough to show.  Say
    // so, because the alternative is that they are simply missing.
    if (s.crowded) {
        const char* more = "drag the edge for more rows";
        ImVec2 ts = ImGui::CalcTextSize(more);
        dl->AddText(ImVec2(s.x + s.w - ts.x - 6.0f, s.y + s.h - ts.y - 4.0f),
                    s.hdr, more);
    }

    if (s.creating) {
        double t0 = s.create_t0 < s.create_t1 ? s.create_t0 : s.create_t1;
        double t1 = s.create_t0 < s.create_t1 ? s.create_t1 : s.create_t0;
        float  x0 = time_to_x(t0, e, s.x, s.w), x1 = time_to_x(t1, e, s.x, s.w);
        if (x1 > x0) {
            dl->AddRectFilled(ImVec2(x0, row_y(s, 0)),
                              ImVec2(x1, row_y(s, 0) + ANN_ROW_H), s.preview);
            dl->AddRect(ImVec2(x0, row_y(s, 0)),
                        ImVec2(x1, row_y(s, 0) + ANN_ROW_H), s.edge_sel, 0.0f, 0, 1.5f);
        }
    }

    if (s.rect_sel) {
        ImGuiIO& io = ImGui::GetIO();
        float x0 = s.rect_x0 < io.MousePos.x ? s.rect_x0 : io.MousePos.x;
        float x1 = s.rect_x0 < io.MousePos.x ? io.MousePos.x : s.rect_x0;
        dl->AddRectFilled(ImVec2(x0, s.y), ImVec2(x1, s.y + s.h), s.band);
        dl->AddRect(ImVec2(x0, s.y), ImVec2(x1, s.y + s.h), s.edge_sel);
    }

    dl->PopClipRect();

    // The resize grip, drawn as what it is: a band you can pull.
    ImGuiIO& io = ImGui::GetIO();
    bool over = io.MousePos.x >= s.x && io.MousePos.x < s.x + s.w &&
                io.MousePos.y >= s.y + s.h - ANN_GRIP_H && io.MousePos.y < s.y + s.h;
    if (over || s.grip) {
        dl->AddRectFilled(ImVec2(s.x, s.y + s.h - ANN_GRIP_H),
                          ImVec2(s.x + s.w, s.y + s.h), s.edge_sel);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
}

void annstrip_edit(AnnStripId id, const AnnStripCtx& c) {
    AnnStrip& s = s_ann[id];
    static int  prev_idx[ANN_COUNT]   = { -1, -1 };
    static bool was_active[ANN_COUNT] = { false, false };

    if (!(s.show && s.editing >= 0 && s.editing < s.map->count)) {
        prev_idx[id]   = -1;
        was_active[id] = false;
        return;
    }

    bool is_new  = (s.editing != prev_idx[id]);
    prev_idx[id] = s.editing;

    MiscAnnotation& ma = s.map->entries[s.editing];
    float x0 = time_to_x(ma.t_start, c.editor, s.x, s.w);
    float x1 = time_to_x(ma.t_end,   c.editor, s.x, s.w);
    float y0 = row_y(s, row_of(s, s.editing));
    float iw = x1 - x0;
    if (iw < 60.0f) iw = 60.0f;

    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::SetNextItemWidth(iw);
    float pad_y = (ANN_ROW_H - ImGui::GetTextLineHeight()) * 0.5f;
    if (pad_y < 1.0f) pad_y = 1.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, pad_y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, s.fill_sel);
    if (is_new) ImGui::SetKeyboardFocusHere(0);
    ImGui::PushID(id);
    bool submitted = ImGui::InputText("##ann_ie", ma.text, sizeof(ma.text),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopID();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    bool active = ImGui::IsItemActive();
    if (submitted || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s.map->dirty = true;
        s.editing    = -1;
    } else if (!active && was_active[id]) {
        s.map->dirty = true;
        s.editing    = -1;
    }
    was_active[id] = active;
}
