#include "ui_stems.h"
#include "stems.h"
#include "ui_timeline.h"
#include "imgui.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#endif

static SpectrogramState* s_mix = nullptr;
void ui_stems_set_mix(SpectrogramState* mix) { s_mix = mix; }

static void tip(const char* s) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s); }

// --- view state ----------------------------------------------------------------

static const float PLANE_HALF_H = 0.5f;    // plane is x in [-1,1], y in [-h,h]

static float s_yaw   = 0.55f;      // radians, around the vertical axis
static float s_pitch = 0.30f;      // radians, above the stack
static float s_dist  = 3.4f;       // camera distance

static float s_spacing = 0.45f;    // depth between planes
static float s_opacity = 1.0f;     // alpha scale
static float s_gamma   = 1.6f;     // alpha curve: alpha = ((i - floor)/(1 - floor))^gamma
static float s_floor   = 0.10f;    // intensity below this is fully transparent
static bool  s_tint    = true;     // tint each plane with its own hue
static bool  s_include_mix = false;
static bool  s_show_3d = false;    // the 3D stack is experimental: off until asked for
static bool  s_show[STEMS_MAX + 1];      // [0] = mix, [1 + i] = stem i
static bool  s_show_init = false;

static void reset_view() { s_yaw = 0.55f; s_pitch = 0.30f; s_dist = 3.4f; }

// Per-stem hues come from the stems module, so the planes match the tabs.
static void stem_tint(const char* name, float* rgb) { stems_tint(name, rgb); }

// --- tiny matrix helpers (column-major, like GL) --------------------------------

struct Mat4 { float m[16]; };

static Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static Mat4 mat_perspective(float fovy, float aspect, float zn, float zf) {
    Mat4 r; memset(r.m, 0, sizeof(r.m));
    float f = 1.0f / tanf(fovy * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zf * zn) / (zn - zf);
    return r;
}

static Mat4 mat_lookat(const float* eye, const float* at, const float* up) {
    float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float fl = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]); for (int i = 0; i < 3; i++) f[i] /= fl;
    float s[3] = { f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0] };
    float sl = sqrtf(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]); for (int i = 0; i < 3; i++) s[i] /= sl;
    float u[3] = { s[1]*f[2] - s[2]*f[1], s[2]*f[0] - s[0]*f[2], s[0]*f[1] - s[1]*f[0] };
    Mat4 r; memset(r.m, 0, sizeof(r.m));
    r.m[0] = s[0]; r.m[4] = s[1]; r.m[8]  = s[2];
    r.m[1] = u[0]; r.m[5] = u[1]; r.m[9]  = u[2];
    r.m[2] = -f[0]; r.m[6] = -f[1]; r.m[10] = -f[2];
    r.m[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    r.m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    r.m[14] =  (f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
    r.m[15] = 1.0f;
    return r;
}

// World point -> pixel inside a w x h viewport (y down).  False when behind the eye.
static bool project(const Mat4& mvp, float x, float y, float z, float w, float h, ImVec2* out) {
    float cx = mvp.m[0]*x + mvp.m[4]*y + mvp.m[8]*z  + mvp.m[12];
    float cy = mvp.m[1]*x + mvp.m[5]*y + mvp.m[9]*z  + mvp.m[13];
    float cw = mvp.m[3]*x + mvp.m[7]*y + mvp.m[11]*z + mvp.m[15];
    if (cw <= 1e-5f) return false;
    out->x = (cx / cw * 0.5f + 0.5f) * w;
    out->y = (1.0f - (cy / cw * 0.5f + 0.5f)) * h;
    return true;
}

// --- GL resources ----------------------------------------------------------------

static GLuint s_prog = 0, s_vao = 0, s_vbo = 0, s_fbo = 0, s_fbo_tex = 0;
static int    s_fbo_w = 0, s_fbo_h = 0;
static GLint  u_mvp, u_z, u_color, u_inten, u_u0, u_u1, u_maxf, u_nyq, u_fmin, u_log,
              u_opacity, u_gamma, u_floor, u_tint;

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp; uniform float u_z;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = a_uv; gl_Position = u_mvp * vec4(a_pos.x, a_pos.y, u_z, 1.0); }\n";

// uv.x spans the timeline view (u0..u1 of the texture); uv.y spans
// 0 Hz (or SPECTRO_LOG_FMIN) .. max_freq, linear or log, like the 2D view.
static const char* FS =
    "#version 330 core\n"
    "in vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D u_color, u_inten;\n"
    "uniform float u_u0, u_u1, u_maxf, u_nyq, u_fmin;\n"
    "uniform int u_log;\n"
    "uniform float u_opacity, u_gamma, u_floor;\n"
    "uniform vec3 u_tint;\n"
    "void main(){\n"
    "  float u = mix(u_u0, u_u1, v_uv.x);\n"
    "  float f = (u_log == 1) ? u_fmin * pow(u_maxf / u_fmin, v_uv.y) : v_uv.y * u_maxf;\n"
    "  float v = 1.0 - f / u_nyq;\n"
    "  vec3  c = texture(u_color, vec2(u, v)).rgb;\n"
    "  float i = texture(u_inten, vec2(u, v)).r;\n"
    "  float a = (i <= u_floor) ? 0.0 : pow((i - u_floor) / (1.0 - u_floor), u_gamma) * u_opacity;\n"
    "  float lum = max(c.r, max(c.g, c.b));\n"
    "  c = mix(c, u_tint * lum, 0.55);\n"
    "  o = vec4(c, a);\n"
    "}\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "[stems3d] shader: %s\n", log);
    }
    return sh;
}

static bool gl_ensure() {
    if (s_prog) return true;
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    GLint ok = 0; glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(s_prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[stems3d] link: %s\n", log);
        glDeleteProgram(s_prog); s_prog = 0;
        return false;
    }
    u_mvp = glGetUniformLocation(s_prog, "u_mvp");   u_z     = glGetUniformLocation(s_prog, "u_z");
    u_color = glGetUniformLocation(s_prog, "u_color"); u_inten = glGetUniformLocation(s_prog, "u_inten");
    u_u0 = glGetUniformLocation(s_prog, "u_u0");     u_u1    = glGetUniformLocation(s_prog, "u_u1");
    u_maxf = glGetUniformLocation(s_prog, "u_maxf"); u_nyq   = glGetUniformLocation(s_prog, "u_nyq");
    u_fmin = glGetUniformLocation(s_prog, "u_fmin"); u_log   = glGetUniformLocation(s_prog, "u_log");
    u_opacity = glGetUniformLocation(s_prog, "u_opacity");
    u_gamma = glGetUniformLocation(s_prog, "u_gamma"); u_floor = glGetUniformLocation(s_prog, "u_floor");
    u_tint = glGetUniformLocation(s_prog, "u_tint");

    // One quad, two triangles: x, y, u, v
    const float h = PLANE_HALF_H;
    const float quad[] = {
        -1, -h, 0, 0,   1, -h, 1, 0,   1, h, 1, 1,
        -1, -h, 0, 0,   1,  h, 1, 1,  -1, h, 0, 1,
    };
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

static bool fbo_ensure(int w, int h) {
    if (s_fbo && s_fbo_w == w && s_fbo_h == h) return true;
    if (s_fbo_tex) glDeleteTextures(1, &s_fbo_tex);
    if (s_fbo)     glDeleteFramebuffers(1, &s_fbo);
    glGenTextures(1, &s_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_fbo_w = w; s_fbo_h = h;
    if (!ok) fprintf(stderr, "[stems3d] framebuffer incomplete\n");
    return ok;
}

// --- the planes --------------------------------------------------------------------

struct Plane { SpectrogramState* spec; const char* name; float z; float dist; };

static int collect_planes(Plane* out) {
    int n = 0, total = 0;
    bool want[STEMS_MAX + 1];
    want[0] = s_include_mix && s_mix && s_mix->computed && s_show[0];
    if (want[0]) total++;
    for (int i = 0; i < stems_count(); i++) {
        want[1 + i] = s_show[1 + i] && stems_spectrogram(i)->computed;
        if (want[1 + i]) total++;
    }
    // Front plane first in the stack order (mix, then stems in model order)
    int k = 0;
    auto place = [&](SpectrogramState* sp, const char* nm) {
        out[n].spec = sp; out[n].name = nm;
        out[n].z = ((total - 1) * 0.5f - k) * s_spacing;
        n++; k++;
    };
    if (want[0]) place(s_mix, "mix");
    for (int i = 0; i < stems_count(); i++)
        if (want[1 + i]) place(stems_spectrogram(i), stems_name(i));
    return n;
}

// Render the stack into the FBO and draw it, with labels, into the current
// window at the cursor, `size` logical pixels.
static void render_3d(ToolCtx& c, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Orbit with the mouse, zoom with the wheel, double-click resets.
    ImGui::InvisibleButton("##orbit", size, ImGuiButtonFlags_MouseButtonLeft);
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        s_yaw   += io.MouseDelta.x * 0.008f;
        s_pitch += io.MouseDelta.y * 0.008f;
        if (s_pitch >  1.50f) s_pitch =  1.50f;
        if (s_pitch < -1.50f) s_pitch = -1.50f;
    }
    if (hov && io.MouseWheel != 0.0f) {
        s_dist *= powf(1.12f, -io.MouseWheel);
        if (s_dist < 1.2f) s_dist = 1.2f;
        if (s_dist > 12.0f) s_dist = 12.0f;
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset_view();

    int fw = (int)(size.x * io.DisplayFramebufferScale.x);
    int fh = (int)(size.y * io.DisplayFramebufferScale.y);
    if (fw < 4 || fh < 4) return;
    if (!gl_ensure() || !fbo_ensure(fw, fh)) {
        dl->AddText(pos, IM_COL32(220, 120, 120, 255), "3D view unavailable (GL)");
        return;
    }

    Plane planes[STEMS_MAX + 1];
    int   np = collect_planes(planes);

    // Camera
    float eye[3] = { s_dist * cosf(s_pitch) * sinf(s_yaw),
                     s_dist * sinf(s_pitch),
                     s_dist * cosf(s_pitch) * cosf(s_yaw) };
    float at[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
    Mat4 mvp = mat_mul(mat_perspective(0.75f, (float)fw / (float)fh, 0.1f, 50.0f),
                       mat_lookat(eye, at, up));

    // Back to front
    for (int i = 0; i < np; i++) {
        float dz = planes[i].z - eye[2];
        planes[i].dist = eye[0]*eye[0] + eye[1]*eye[1] + dz*dz;
    }
    std::sort(planes, planes + np, [](const Plane& a, const Plane& b) { return a.dist > b.dist; });

    // Frequency axis: the timeline's controls
    int  max_khz = 22; bool log_freq = false;
    ui_timeline_spectro_view(&max_khz, &log_freq);
    double vs = c.editor->view_start, ve = c.editor->view_end;

    GLint prev_fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, fw, fh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.07f, 0.07f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
    glUniform1i(u_color, 0);
    glUniform1i(u_inten, 1);
    glUniform1f(u_opacity, s_opacity);
    glUniform1f(u_gamma, s_gamma);
    glUniform1f(u_floor, s_floor);
    glUniform1f(u_fmin, SPECTRO_LOG_FMIN);
    glUniform1i(u_log, log_freq ? 1 : 0);
    for (int i = 0; i < np; i++) {
        SpectrogramState* sp = planes[i].spec;
        float nyq  = sp->sample_rate > 0 ? (float)(sp->sample_rate / 2) : 22050.0f;
        float maxf = (float)(max_khz * 1000);
        if (maxf > nyq) maxf = nyq;
        float u0 = sp->duration > 0 ? (float)(vs / sp->duration) : 0.0f;
        float u1 = sp->duration > 0 ? (float)(ve / sp->duration) : 1.0f;
        float tint[3] = { 1, 1, 1 };
        if (s_tint) stem_tint(planes[i].name, tint);
        glUniform1f(u_z, planes[i].z);
        glUniform1f(u_u0, u0); glUniform1f(u_u1, u1);
        glUniform1f(u_maxf, maxf); glUniform1f(u_nyq, nyq);
        glUniform3f(u_tint, tint[0], tint[1], tint[2]);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, sp->texture);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, sp->intensity);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    {
        static bool reported = false;
        GLenum err = glGetError();
        if (err != GL_NO_ERROR && !reported) {
            fprintf(stderr, "[stems3d] GL error 0x%x\n", (unsigned)err);
            reported = true;
        }
    }

    // The rendered stack (FBO rows are bottom-up, so flip v)
    dl->AddImage((ImTextureID)(intptr_t)s_fbo_tex, pos, ImVec2(pos.x + size.x, pos.y + size.y),
                 ImVec2(0, 1), ImVec2(1, 0));

    // Overlay: plane outlines, playhead, labels -- projected by hand
    const float h = PLANE_HALF_H;
    double span = ve - vs;
    double ph   = audio_get_position(c.audio);
    float  pfrac = span > 0 ? (float)((ph - vs) / span) : -1.0f;
    for (int i = 0; i < np; i++) {
        float z = planes[i].z;
        ImVec2 q[4];
        bool ok = project(mvp, -1, -h, z, size.x, size.y, &q[0]) &&
                  project(mvp,  1, -h, z, size.x, size.y, &q[1]) &&
                  project(mvp,  1,  h, z, size.x, size.y, &q[2]) &&
                  project(mvp, -1,  h, z, size.x, size.y, &q[3]);
        if (!ok) continue;
        for (int k = 0; k < 4; k++) q[k] = ImVec2(q[k].x + pos.x, q[k].y + pos.y);
        float tint[3] = { 1, 1, 1 };
        if (s_tint) stem_tint(planes[i].name, tint);
        ImU32 col = IM_COL32((int)(tint[0] * 200), (int)(tint[1] * 200), (int)(tint[2] * 200), 110);
        dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, 1.0f);
        if (pfrac >= 0.0f && pfrac <= 1.0f) {
            float px = -1.0f + 2.0f * pfrac;
            ImVec2 a, b;
            if (project(mvp, px, -h, z, size.x, size.y, &a) && project(mvp, px, h, z, size.x, size.y, &b))
                dl->AddLine(ImVec2(a.x + pos.x, a.y + pos.y), ImVec2(b.x + pos.x, b.y + pos.y),
                            IM_COL32(255, 160, 40, 200), 1.5f);
        }
        ImVec2 ts = ImGui::CalcTextSize(planes[i].name);
        ImVec2 lp(q[3].x + 3.0f, q[3].y - ts.y - 1.0f);
        dl->AddRectFilled(ImVec2(lp.x - 2, lp.y - 1), ImVec2(lp.x + ts.x + 2, lp.y + ts.y + 1),
                          IM_COL32(10, 10, 16, 170), 2.0f);
        dl->AddText(lp, IM_COL32((int)(tint[0] * 255), (int)(tint[1] * 255), (int)(tint[2] * 255), 255),
                    planes[i].name);
    }
    if (np == 0) {
        const char* msg = "No stems to show";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                    IM_COL32(120, 120, 150, 255), msg);
    }
}

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

// --- tool parts -----------------------------------------------------------------------

void ui_stems_settings(ToolCtx& c)
{
    (void)c;
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Checkbox("3D stack (experimental)", &s_show_3d);
    tip("Show every stem's spectrogram as a translucent plane in depth");
    if (!s_show_3d) return;
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##spacing", &s_spacing, 0.05f, 1.5f, "Plane spacing %.2f");
    tip("Depth between neighbouring planes");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##opacity", &s_opacity, 0.1f, 1.0f, "Opacity %.2f");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##gamma", &s_gamma, 0.3f, 4.0f, "Alpha curve %.1f");
    tip("Alpha = magnitude ^ curve: higher keeps only the strong energy");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##floor", &s_floor, 0.0f, 0.6f, "Floor %.2f");
    tip("Magnitude (0..1 over -80..0 dB) below which a pixel is fully transparent");
    ImGui::Checkbox("Tint planes by stem", &s_tint);
    ImGui::Checkbox("Include the mix as a plane", &s_include_mix);
    if (ImGui::Button("Reset view", ImVec2(w, 0))) reset_view();
    tip("Also: double-click the view.  Drag to orbit, wheel to zoom.");
}

void ui_stems_body(ToolCtx& c)
{
    if (!s_show_init) { for (int i = 0; i <= STEMS_MAX; i++) s_show[i] = true; s_show_init = true; }

    StemsStatus st = stems_status();
    if (st == STEMS_RUNNING || st == STEMS_CHECKING) {
        ImGui::ProgressBar(stems_progress(), ImVec2(-FLT_MIN, 0), stems_message());
    } else if (st == STEMS_FAILED) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.5f, 1.0f), "%s", stems_message());
    } else if (st == STEMS_READY && s_show_3d) {
        // Per-plane toggles in one row
        if (s_include_mix) { ImGui::Checkbox("mix", &s_show[0]); ImGui::SameLine(); }
        for (int i = 0; i < stems_count(); i++) {
            ImGui::PushID(i);
            ImGui::Checkbox(stems_name(i), &s_show[1 + i]);
            ImGui::PopID();
            if (i + 1 < stems_count()) ImGui::SameLine();
        }
    } else if (st == STEMS_READY) {
        // Stem list with the tab colours; the tabs above the spectrogram
        // pick what plays and shows.
        ImGui::TextDisabled("%d stems.  The tabs above the spectrogram choose which\n"
                            "play and show; any subset, each in its own colour.", stems_count());
        ImGui::Spacing();
        for (int i = 0; i < stems_count(); i++) {
            float tint[3]; stems_tint(stems_name(i), tint);
            bool sel = stems_is_selected(i);
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(tint[0], tint[1], tint[2], sel ? 1.0f : 0.55f));
            if (ImGui::Checkbox(stems_name(i), &sel)) stems_toggle(i);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (stems_rebuilding()) ImGui::TextDisabled("mixing\xe2\x80\xa6");
        return;
    } else {
        ImGui::TextDisabled(c.audio->loaded ? "Separate stems to play and view the parts of the mix."
                                            : "Load a track first.");
    }
    if (!s_show_3d) return;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 8.0f || avail.y < 8.0f) return;
    render_3d(c, avail);
}

void ui_stems_actions(ToolCtx& c)
{
    float w = ImGui::GetContentRegionAvail().x;
    StemsStatus st = stems_status();
    bool busy = (st == STEMS_RUNNING || st == STEMS_CHECKING);
    bool can  = c.audio->loaded && !busy && stems_available();
    if (!can) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.35f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.45f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
    if (ImGui::Button(st == STEMS_READY ? "Separate again" : "Separate stems", ImVec2(w, 0)))
        stems_request(c.audio->filename, false);
    ImGui::PopStyleColor(3);
    if (!can) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!stems_available())
            ImGui::SetTooltip("Separator not installed: run `make stems-model` (scripts/setup-stems.sh)");
        else
            ImGui::SetTooltip("Split the track into vocals, drums, bass, guitar, piano and other\n"
                              "with Demucs (htdemucs_6s).  Cached per track after the first run.");
    }
}
