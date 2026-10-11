#include "ui_stems.h"
#include "stems.h"
#include "imgui.h"
#include <stdio.h>

static void tip(const char* s) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s); }

// --- analysis source widgets ---------------------------------------------------------

bool ui_stems_source_row(const char* label, StemSource* src, StemPreset preset)
{
    if (stems_count() == 0) return false;
    uint32_t mask = stems_source_mask(src, preset);
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 3.0f));
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool mix = (mask == 0);
    ImGui::PushStyleColor(ImGuiCol_Button,        mix ? IM_COL32(60, 75, 115, 255) : IM_COL32(30, 30, 46, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 90, 135, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, mix ? IM_COL32(240, 242, 255, 255) : IM_COL32(150, 152, 175, 255));
    if (ImGui::SmallButton("mix")) { src->custom = true; src->mask = 0; changed = true; }
    ImGui::PopStyleColor(3);
    tip("Analyze the whole mix");
    for (int i = 0; i < stems_count(); i++) {
        const char* name = stems_name(i);
        bool on = (mask >> i) & 1u;
        float t[3]; stems_tint(name, t);
        ImVec2 sz = ImGui::CalcTextSize(name);
        if (ImGui::GetCursorScreenPos().x + sz.x + 12.0f > right) ImGui::NewLine();
        else ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,        on ? IM_COL32(55, 70, 110, 255) : IM_COL32(30, 30, 46, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 90, 135, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32((int)(t[0] * (on ? 255 : 140)),
                                                      (int)(t[1] * (on ? 255 : 140)),
                                                      (int)(t[2] * (on ? 255 : 140)), 255));
        if (ImGui::SmallButton(name)) {
            src->custom = true;
            src->mask   = mask ^ (1u << i);
            changed = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the %s stem in what this tool analyzes", name);
    }
    if (src->custom && src->mask != stems_preset_mask(preset)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("default")) { src->custom = false; changed = true; }
        tip("Back to this tool's default stems");
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

void ui_stems_source_note(StemSource* src, StemPreset preset, const char* verb)
{
    if (stems_count() == 0) return;
    uint32_t mask = stems_source_mask(src, preset);
    if (mask == 0) return;
    char lbl[128]; stems_source_label(mask, lbl, sizeof(lbl));
    ImGui::TextDisabled("%s %s", verb, lbl);
    tip("The stems this tool analyzes (settings to change)");
}
