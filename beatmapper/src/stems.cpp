#include "stems.h"
#include "audio.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern char** environ;

// --- state -----------------------------------------------------------------

struct Stem {
    char             name[32];
    char             path[512];
    SpectrogramState spec;
    uint8_t*         inten;      // magnitude pixels kept for the composite
    int              w, h;
};

static Stem        s_stems[STEMS_MAX];
static int         s_count = 0;
static AudioState* s_audio = nullptr;

static std::atomic<int>   s_status{STEMS_IDLE};
static std::atomic<float> s_progress{0.0f};
static std::mutex         s_msg_mu;
static std::string        s_msg;

// Separation worker: one run at a time.  `s_gen` is bumped by stems_reset so
// a run that finishes after the track changed is discarded rather than landed.
struct WorkStem { std::string name, path; SpectrogramPixels px; bool ok; };
static std::thread            s_worker;
static std::atomic<bool>      s_running{false};
static std::atomic<bool>      s_done{false};
static std::atomic<int>       s_gen{0};
static int                    s_work_gen = 0;
static std::vector<WorkStem>  s_work;
static std::atomic<pid_t>     s_child{0};
static std::string            s_work_audio;
static std::string            s_last_audio;      // for a retry once the venv appears
static std::string            s_pending_names;   // --stems: applied when stems land
static bool                   s_not_installed = false;

// Selection and the work it triggers (composite image + summed audio).
static uint32_t               s_sel_mask = 0;
static int                    s_stems_gen = 0;    // bumps on land / drop

// Analysis-source audio, one mono buffer per mask, kept until drop_stems.
struct SourceBuf { uint32_t mask; float* pcm; uint64_t frames; };
static std::vector<SourceBuf> s_sources;
static SpectrogramState       s_composite;
static std::thread            s_sel_worker;
static std::atomic<bool>      s_sel_running{false};
static std::atomic<bool>      s_sel_done{false};
static bool                   s_sel_dirty = false;
static uint32_t               s_sel_work_mask = 0;
static int                    s_sel_work_gen = 0;
static SpectrogramPixels      s_sel_px;             // worker result: image
static float*                 s_sel_pcm = nullptr;  // worker result: audio
static uint64_t               s_sel_frames = 0;

static void set_msg(const char* m) {
    std::lock_guard<std::mutex> g(s_msg_mu);
    s_msg = m;
}

// A cached stem is <name>.wav until the background compressor (spawned by
// spawn_compressor) swaps it for <name>.mp3, renaming the mp3 into place
// before removing the wav.  A path read earlier may name the wav that has
// since gone, so a failed decode retries with the other extension.
static std::string stem_alt_path(const char* path) {
    std::string p = path;
    size_t dot = p.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = p.substr(dot);
    if (ext == ".wav") return p.substr(0, dot) + ".mp3";
    if (ext == ".mp3") return p.substr(0, dot) + ".wav";
    return "";
}

static bool decode_stem_at(const char* path, uint32_t sr, float** pcm, uint64_t* frames) {
    if (audio_decode_stereo_at(path, sr, pcm, frames)) return true;
    std::string alt = stem_alt_path(path);
    return !alt.empty() && audio_decode_stereo_at(alt.c_str(), sr, pcm, frames);
}

static bool decode_stem_mono(const char* path, float** pcm, uint64_t* frames, uint32_t* sr) {
    if (audio_decode_pcm(path, pcm, frames, sr)) return true;
    std::string alt = stem_alt_path(path);
    return !alt.empty() && audio_decode_pcm(alt.c_str(), pcm, frames, sr);
}

void stems_tint(const char* name, float* rgb) {
    struct { const char* n; float r, g, b; } T[] = {
        { "vocals", 1.00f, 0.30f, 0.60f }, { "drums",  0.30f, 0.80f, 1.00f },
        { "bass",   0.60f, 0.45f, 1.00f }, { "guitar", 1.00f, 0.72f, 0.20f },
        { "piano",  0.35f, 1.00f, 0.45f }, { "other",  0.90f, 0.90f, 0.90f },
        { "mix",    1.00f, 1.00f, 1.00f },
    };
    for (auto& t : T) if (strcmp(t.n, name) == 0) { rgb[0] = t.r; rgb[1] = t.g; rgb[2] = t.b; return; }
    rgb[0] = rgb[1] = rgb[2] = 1.0f;
}

// --- locating the script -----------------------------------------------------

// The app is not always launched with the repo as its working directory
// (the .app shim does cd there, a bare launch may not), so fall back to
// paths relative to the executable -- its real path, since the binary is
// often reached through a symlink such as ~/bin/beatmapper.
static bool resolve_tool(std::string* python, std::string* script) {
    const char* env = getenv("BM_STEMS_CMD");
    if (env && env[0]) { *python = env; script->clear(); return true; }
    std::string bases[2] = { "", "" };
    int nb = 1;
    char exe[4096];
    if (platform_exe_path(exe, sizeof(exe))) {
        char* slash = strrchr(exe, '/');
        if (slash) { slash[1] = 0; bases[nb++] = exe; }
    }
    for (int i = 0; i < nb; i++) {
        std::string py = bases[i] + "external/venv-stems/bin/python";
        std::string sc = bases[i] + "scripts/stems_demucs.py";
        if (access(py.c_str(), X_OK) == 0 && access(sc.c_str(), R_OK) == 0) {
            *python = py; *script = sc; return true;
        }
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        fprintf(stderr, "[stems] separator not found: tried external/venv-stems/bin/python under");
        for (int i = 0; i < nb; i++) fprintf(stderr, " '%s'", bases[i].empty() ? "./" : bases[i].c_str());
        fprintf(stderr, " (set BM_STEMS_CMD to override)\n");
    }
    return false;
}

bool stems_available() {
    std::string py, sc;
    return resolve_tool(&py, &sc);
}

// --- background compression ------------------------------------------------

// Demucs writes the cache as 16-bit wav (~10 MB/min per stem); once the stems
// are loaded, scripts/stems_compress.py re-encodes them as CBR mp3 at the
// source's bit rate.  It runs detached in its own session at low priority,
// outlives a track change or quit, and locks the directory itself, so a
// second request while one runs is harmless.  A reaper thread collects it.
static void spawn_compressor(const std::string& python, const std::string& script,
                             const std::string& stem_dir, const std::string& audio,
                             const std::string& log) {
    std::string sc = script.substr(0, script.rfind('/') + 1) + "stems_compress.py";
    if (access(sc.c_str(), R_OK) != 0) return;
    std::vector<std::string> args = { python, sc, stem_dir, audio };
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, log.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#endif
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, argv.data(), environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { fprintf(stderr, "[stems] could not start %s\n", sc.c_str()); return; }
    printf("[stems] compressing %s in the background (pid %d)\n", stem_dir.c_str(), (int)pid);
    std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
}

// --- separation worker -------------------------------------------------------

static void worker_main(std::string audio, bool cached_only, int gen) {
    std::string py, sc;
    if (!resolve_tool(&py, &sc)) {
        set_msg("Separator not installed: run scripts/setup-stems.sh");
        s_status.store(STEMS_FAILED);
        s_done.store(true);
        return;
    }

    // Build argv.  BM_STEMS_CMD may hold "python script" as one string.
    std::vector<std::string> args;
    if (sc.empty()) {
        size_t p = 0;
        while (p < py.size()) {
            size_t q = py.find(' ', p);
            if (q == std::string::npos) q = py.size();
            if (q > p) args.push_back(py.substr(p, q - p));
            p = q + 1;
        }
    } else {
        args.push_back(py);
        args.push_back(sc);
    }
    args.push_back(audio);
    if (cached_only) args.push_back("--check");
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    // stderr goes to a log the user can be pointed at
    std::string log = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.cache/beatmapper/stems";
    std::string mk = "mkdir -p \"" + log + "\"";
    if (system(mk.c_str()) != 0) {}
    log += "/log.txt";

    int fds[2];
    if (pipe(fds) != 0) {
        set_msg("Could not start the separator (pipe)");
        s_status.store(STEMS_FAILED); s_done.store(true); return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, log.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        set_msg("Could not start the separator");
        s_status.store(STEMS_FAILED); s_done.store(true); return;
    }
    s_child.store(pid);

    std::vector<WorkStem> found;
    FILE* f = fdopen(fds[0], "r");
    char line[1024];
    while (f && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char* rest = tab + 1;
        if (strcmp(line, "progress") == 0) {
            s_progress.store((float)atof(rest));
            s_status.store(STEMS_RUNNING);
        } else if (strcmp(line, "status") == 0) {
            set_msg(rest);
            s_status.store(STEMS_RUNNING);
        } else if (strcmp(line, "stem") == 0) {
            char* tab2 = (char*)strchr(rest, '\t');
            if (!tab2) continue;
            *tab2 = 0;
            WorkStem ws; ws.name = rest; ws.path = tab2 + 1; ws.ok = false;
            ws.px.rgba = nullptr; ws.px.intensity = nullptr; ws.px.tex_w = ws.px.tex_h = 0;
            if ((int)found.size() < STEMS_MAX) found.push_back(ws);
        }
    }
    if (f) fclose(f); else close(fds[0]);
    int wstat = 0;
    waitpid(pid, &wstat, 0);
    s_child.store(0);
    int code = WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1;

    if (s_gen.load() != gen) {            // track changed meanwhile
        s_done.store(true);
        return;
    }
    if (code == 3 && cached_only) {        // not cached: nothing to show yet
        set_msg("Not separated yet");
        s_status.store(STEMS_IDLE);
        s_done.store(true);
        return;
    }
    if (code != 0 || found.empty()) {
        std::lock_guard<std::mutex> g(s_msg_mu);
        if (s_msg.rfind("failed", 0) != 0)
            s_msg = "Separation failed (see ~/.cache/beatmapper/stems/log.txt)";
        s_status.store(STEMS_FAILED);
        s_done.store(true);
        return;
    }

    // Decode each stem and build its spectrogram pixels here, off the GL thread.
    for (size_t i = 0; i < found.size(); i++) {
        if (s_gen.load() != gen) break;
        char m[128];
        snprintf(m, sizeof(m), "Building spectrogram %d/%d (%s)", (int)i + 1, (int)found.size(),
                 found[i].name.c_str());
        set_msg(m);
        s_progress.store((float)i / (float)found.size());
        float*   pcm = nullptr;
        uint64_t nfr = 0;
        uint32_t sr  = 0;
        if (decode_stem_mono(found[i].path.c_str(), &pcm, &nfr, &sr)) {
            found[i].ok = spectrogram_compute_pixels(&found[i].px, pcm, nfr, sr);
            audio_free_pcm(pcm);
        }
    }
    // Loaded: any stems still in wav get compressed off the critical path.
    if (!sc.empty() && s_gen.load() == gen) {
        for (const WorkStem& w : found) {
            size_t n = w.path.size();
            if (n > 4 && w.path.compare(n - 4, 4, ".wav") == 0) {
                spawn_compressor(py, sc, w.path.substr(0, w.path.rfind('/')), audio, log);
                break;
            }
        }
    }
    s_work = std::move(found);
    s_done.store(true);
}

static void worker_join_discard() {
    if (s_worker.joinable()) {
        pid_t pid = s_child.load();
        if (pid > 0) kill(pid, SIGTERM);
        s_worker.join();
    }
    for (WorkStem& w : s_work) spectrogram_pixels_free(&w.px);
    s_work.clear();
    s_running.store(false);
    s_done.store(false);
}

// --- selection worker ----------------------------------------------------------
// Builds, for the selected stems, the summed per-stem-coloured image and the
// summed audio at the track's sample rate.  Reads only the stems' retained
// magnitude pixels and file paths, which are fixed until stems_reset joins it.

static void sel_free_results() {
    spectrogram_pixels_free(&s_sel_px);
    free(s_sel_pcm); s_sel_pcm = nullptr; s_sel_frames = 0;
}

// Per-stem ramp over the -80..0 dB magnitude: black below a floor (so a
// stem's noise bed does not fog the others), a steep rise through the
// stem's colour, then on past full saturation toward white, so the strong
// energy of each stem reads as a bright highlight in its own hue.
static void stem_ramp(float v, const float* tint, float* rgb) {
    const float floor_v = 0.16f;
    if (v <= floor_v) { rgb[0] = rgb[1] = rgb[2] = 0.0f; return; }
    float t = (v - floor_v) / (1.0f - floor_v);
    float b = 1.8f * powf(t, 1.2f);          // > 1 above ~60 % of the way up
    float sat = b < 1.0f ? b : 1.0f;
    float hi  = b > 1.0f ? (b - 1.0f) * 0.7f : 0.0f;
    for (int k = 0; k < 3; k++) {
        float c = tint[k] * sat + hi * (1.0f - tint[k]);
        rgb[k] = c > 1.0f ? 1.0f : c;
    }
}

static void sel_worker_main(uint32_t mask, uint32_t sample_rate, int gen) {
    // 1. image
    int w = 0, h = 0, first = -1;
    for (int i = 0; i < s_count; i++)
        if ((mask >> i) & 1u) { w = s_stems[i].w; h = s_stems[i].h; first = i; break; }
    s_sel_px.rgba = nullptr; s_sel_px.intensity = nullptr; s_sel_px.tex_w = s_sel_px.tex_h = 0;
    if (w > 0 && h > 0) {
        size_t n = (size_t)w * h;
        uint16_t* acc = (uint16_t*)calloc(n * 3, sizeof(uint16_t));
        uint8_t*  out = (uint8_t*)malloc(n * 4);
        if (acc && out) {
            for (int i = 0; i < s_count; i++) {
                if (!((mask >> i) & 1u) || !s_stems[i].inten) continue;
                if (s_stems[i].w != w || s_stems[i].h != h) continue;
                if (s_gen.load() != gen) break;
                float tint[3]; stems_tint(s_stems[i].name, tint);
                uint16_t lut[256][3];
                for (int v = 0; v < 256; v++) {
                    float rgb[3]; stem_ramp((float)v / 255.0f, tint, rgb);
                    for (int k = 0; k < 3; k++) lut[v][k] = (uint16_t)(rgb[k] * 255.0f + 0.5f);
                }
                const uint8_t* src = s_stems[i].inten;
                for (size_t p = 0; p < n; p++) {
                    const uint16_t* l = lut[src[p]];
                    acc[p * 3 + 0] += l[0]; acc[p * 3 + 1] += l[1]; acc[p * 3 + 2] += l[2];
                }
            }
            for (size_t p = 0; p < n; p++) {
                for (int k = 0; k < 3; k++) {
                    uint16_t a = acc[p * 3 + k];
                    out[p * 4 + k] = (uint8_t)(a > 255 ? 255 : a);
                }
                out[p * 4 + 3] = 255;
            }
            s_sel_px.rgba = out; out = nullptr;
            s_sel_px.tex_w = w; s_sel_px.tex_h = h;
            s_sel_px.duration    = s_stems[first].spec.duration;
            s_sel_px.sample_rate = s_stems[first].spec.sample_rate;
        }
        free(acc); free(out);
    }

    // 2. audio: decode each selected stem at the track's rate and sum
    float* sum = nullptr; uint64_t sum_frames = 0;
    for (int i = 0; i < s_count; i++) {
        if (!((mask >> i) & 1u)) continue;
        if (s_gen.load() != gen) break;
        float* pcm = nullptr; uint64_t fr = 0;
        if (!decode_stem_at(s_stems[i].path, sample_rate, &pcm, &fr)) continue;
        if (!sum) { sum = pcm; sum_frames = fr; continue; }
        uint64_t n = fr < sum_frames ? fr : sum_frames;
        for (uint64_t k = 0; k < n * 2; k++) sum[k] += pcm[k];
        free(pcm);
    }
    s_sel_pcm = sum; s_sel_frames = sum_frames;
    s_sel_done.store(true);
}

static void sel_join_discard() {
    if (s_sel_worker.joinable()) s_sel_worker.join();
    sel_free_results();
    s_sel_running.store(false);
    s_sel_done.store(false);
}

static void sel_start() {
    s_sel_dirty = false;
    if (s_sel_mask == 0 || !s_audio) return;
    uint32_t ch = 0, sr = 0; uint64_t fr = 0;
    audio_pcm_data(s_audio, &fr, &ch, &sr);
    if (sr == 0) return;
    s_sel_work_mask = s_sel_mask;
    s_sel_work_gen  = s_gen.load();
    s_sel_running.store(true);
    s_sel_done.store(false);
    s_sel_worker = std::thread(sel_worker_main, s_sel_mask, sr, s_sel_work_gen);
}

static void sel_changed() {
    if (s_sel_mask == 0) {
        // The mix: no image to build, playback goes straight back
        if (s_audio) audio_set_playback_override(s_audio, nullptr, 0);
        spectrogram_shutdown(&s_composite);
        s_sel_dirty = false;
        return;
    }
    if (s_sel_running.load()) { s_sel_dirty = true; return; }
    sel_join_discard();
    sel_start();
}

// A new track loaded this frame but stems_reset has not run yet (the main
// loop resets on the frame after audio_load): anything in flight is stale.
static bool track_changed() {
    return s_audio && strcmp(s_audio->filename, s_last_audio.c_str()) != 0;
}

// --- public ------------------------------------------------------------------

void stems_init(AudioState* audio) {
    s_audio = audio;
    for (int i = 0; i < STEMS_MAX; i++) {
        spectrogram_init(&s_stems[i].spec);
        s_stems[i].inten = nullptr; s_stems[i].w = s_stems[i].h = 0;
    }
    spectrogram_init(&s_composite);
    s_sel_px.rgba = nullptr; s_sel_px.intensity = nullptr; s_sel_px.tex_w = s_sel_px.tex_h = 0;
    s_count = 0; s_sel_mask = 0;
    s_status.store(STEMS_IDLE);
    set_msg("");
}

static void drop_stems() {
    for (int i = 0; i < s_count; i++) {
        spectrogram_shutdown(&s_stems[i].spec);
        free(s_stems[i].inten); s_stems[i].inten = nullptr;
    }
    for (SourceBuf& b : s_sources) free(b.pcm);
    s_sources.clear();
    if (s_count) s_stems_gen++;
    s_count = 0;
    s_sel_mask = 0;
    spectrogram_shutdown(&s_composite);
}

void stems_shutdown() {
    s_gen.fetch_add(1);
    if (s_worker.joinable()) {
        // Never block the exit on the model: kill the child, let the thread
        // notice and leave.  Its pixel buffers die with the process.
        pid_t pid = s_child.load();
        if (pid > 0) kill(pid, SIGTERM);
        s_worker.detach();
    }
    if (s_sel_worker.joinable()) s_sel_worker.join();
    sel_free_results();
    drop_stems();
}

void stems_reset() {
    s_gen.fetch_add(1);
    worker_join_discard();
    sel_join_discard();
    s_sel_dirty = false;
    drop_stems();
    if (s_audio) audio_set_playback_override(s_audio, nullptr, 0);
    s_not_installed = false;
    s_status.store(STEMS_IDLE);
    s_progress.store(0.0f);
    set_msg("");
}

void stems_request(const char* audio_path, bool cached_only) {
    if (!audio_path || !audio_path[0]) return;
    if (s_running.load()) return;
    s_last_audio = audio_path;
    if (!stems_available()) {
        set_msg("Separator not installed: run `make stems-model`");
        s_status.store(STEMS_FAILED);
        s_not_installed = true;
        return;
    }
    s_not_installed = false;
    worker_join_discard();
    s_work_gen   = s_gen.load();
    s_work_audio = audio_path;
    s_progress.store(0.0f);
    s_status.store(cached_only ? STEMS_CHECKING : STEMS_RUNNING);
    set_msg(cached_only ? "Looking for cached stems" : "Starting separator");
    s_running.store(true);
    s_done.store(false);
    s_worker = std::thread(worker_main, s_work_audio, cached_only, s_work_gen);
}

static void land_separation() {
    if (s_worker.joinable()) s_worker.join();
    s_running.store(false);
    s_done.store(false);
    if (s_work_gen != s_gen.load() || track_changed()) {   // stale: the track changed
        for (WorkStem& w : s_work) spectrogram_pixels_free(&w.px);
        s_work.clear();
        return;
    }
    if (s_work.empty()) return;            // failed / not cached: status already set
    sel_join_discard();
    if (s_audio) audio_set_playback_override(s_audio, nullptr, 0);
    drop_stems();
    for (WorkStem& w : s_work) {
        if (!w.ok || s_count >= STEMS_MAX) { spectrogram_pixels_free(&w.px); continue; }
        Stem& st = s_stems[s_count];
        strncpy(st.name, w.name.c_str(), sizeof(st.name) - 1); st.name[sizeof(st.name) - 1] = 0;
        strncpy(st.path, w.path.c_str(), sizeof(st.path) - 1); st.path[sizeof(st.path) - 1] = 0;
        // Keep the magnitude pixels: the composite is summed from them.
        st.w = w.px.tex_w; st.h = w.px.tex_h;
        st.inten = (uint8_t*)malloc((size_t)st.w * st.h);
        if (st.inten) memcpy(st.inten, w.px.intensity, (size_t)st.w * st.h);
        spectrogram_upload(&st.spec, &w.px);
        if (st.spec.computed) s_count++;
        else { free(st.inten); st.inten = nullptr; }
    }
    s_work.clear();
    s_stems_gen++;
    char m[64];
    snprintf(m, sizeof(m), "%d stems", s_count);
    set_msg(m);
    s_progress.store(1.0f);
    s_status.store(s_count > 0 ? STEMS_READY : STEMS_FAILED);

    // A launch-time selection (--stems vocals,drums)
    if (!s_pending_names.empty()) {
        uint32_t m2 = 0;
        size_t p = 0;
        while (p < s_pending_names.size()) {
            size_t q = s_pending_names.find(',', p);
            if (q == std::string::npos) q = s_pending_names.size();
            std::string nm = s_pending_names.substr(p, q - p);
            for (int i = 0; i < s_count; i++) if (nm == s_stems[i].name) m2 |= 1u << i;
            p = q + 1;
        }
        s_pending_names.clear();
        if (m2 != s_sel_mask) { s_sel_mask = m2; sel_changed(); }
    }
}

static void land_selection() {
    if (s_sel_worker.joinable()) s_sel_worker.join();
    s_sel_running.store(false);
    s_sel_done.store(false);
    bool stale = (s_sel_work_gen != s_gen.load()) || (s_sel_work_mask != s_sel_mask) ||
                 track_changed();
    if (stale) {
        sel_free_results();
    } else {
        printf("[stems] selection 0x%x: image %dx%d, audio %llu frames\n",
               (unsigned)s_sel_mask, s_sel_px.tex_w, s_sel_px.tex_h, (unsigned long long)s_sel_frames);
        if (s_sel_px.rgba) spectrogram_upload(&s_composite, &s_sel_px);
        else spectrogram_pixels_free(&s_sel_px);
        if (s_audio && s_sel_pcm) {
            audio_set_playback_override(s_audio, s_sel_pcm, s_sel_frames);   // takes ownership
            s_sel_pcm = nullptr; s_sel_frames = 0;
        }
    }
    if (s_sel_dirty) sel_changed();
}

void stems_update() {
    // Installed since the last look (make stems-model while the app ran):
    // ask the cache again so the tool comes back without a reload.
    if (s_not_installed && !s_running.load() && stems_available()) {
        s_not_installed = false;
        stems_request(s_last_audio.c_str(), true);
        return;
    }
    if (s_running.load() && s_done.load()) land_separation();
    if (s_sel_running.load() && s_sel_done.load()) land_selection();

    // BM_STEMS_DEBUG_CYCLE: flip the first stem every ~2 s, to exercise the
    // playback buffer hand-over from the command line.
    static int cycle = -1;
    if (cycle < 0) cycle = getenv("BM_STEMS_DEBUG_CYCLE") ? 0 : 0x7fffffff;
    if (cycle != 0x7fffffff && s_count > 0 && ++cycle % 120 == 0) stems_toggle(0);
}

StemsStatus stems_status()   { return (StemsStatus)s_status.load(); }
float       stems_progress() { return s_progress.load(); }
const char* stems_message() {
    static char buf[256];
    std::lock_guard<std::mutex> g(s_msg_mu);
    strncpy(buf, s_msg.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    return buf;
}

int stems_count() { return s_count; }
const char* stems_name(int i) { return (i >= 0 && i < s_count) ? s_stems[i].name : ""; }
SpectrogramState* stems_spectrogram(int i) {
    return (i >= 0 && i < s_count) ? &s_stems[i].spec : nullptr;
}

bool     stems_mix_selected()      { return s_sel_mask == 0; }
bool     stems_is_selected(int i)  { return i >= 0 && i < s_count && ((s_sel_mask >> i) & 1u); }
uint32_t stems_selection_mask()    { return s_sel_mask; }
void stems_toggle(int i) {
    if (i < 0 || i >= s_count) return;
    s_sel_mask ^= (1u << i);
    sel_changed();
}
void stems_select_mix()  { if (s_sel_mask) { s_sel_mask = 0; sel_changed(); } }
void stems_select_only(int i) {
    if (i < 0 || i >= s_count) return;
    uint32_t m = 1u << i;
    if (m != s_sel_mask) { s_sel_mask = m; sel_changed(); }
}
void stems_select_names(const char* csv) { s_pending_names = csv ? csv : ""; }

// --- analysis sources -----------------------------------------------------------

int stems_generation() { return s_stems_gen; }

static uint32_t mask_of(const char* const* names, int n) {
    uint32_t m = 0;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < s_count; i++)
            if (strcmp(s_stems[i].name, names[k]) == 0) m |= 1u << i;
    return m;
}

uint32_t stems_preset_mask(StemPreset preset) {
    // Bench (18 tracks): everything-but-drums 84.1 % chord beats vs 83.4 %
    // for the mix; guitar+piano alone 78.9 %.  Vocals and "other" carry
    // harmony too.
    static const char* HARM[] = { "vocals", "guitar", "piano", "other", "bass" };
    static const char* RHYT[] = { "drums", "bass" };
    static const char* DRUM[] = { "drums" };
    switch (preset) {
    case STEM_SRC_HARMONIC: return mask_of(HARM, 5);
    case STEM_SRC_RHYTHM:   return mask_of(RHYT, 2);
    case STEM_SRC_DRUMS:    return mask_of(DRUM, 1);
    default:                return 0;
    }
}

uint32_t stems_source_mask(StemSource* src, StemPreset preset) {
    if (src->gen != s_stems_gen) {          // new stem set: back to the preset
        src->gen    = s_stems_gen;
        src->custom = false;
        src->mask   = stems_preset_mask(preset);
    }
    if (!src->custom) src->mask = stems_preset_mask(preset);
    uint32_t valid = s_count >= 32 ? 0xffffffffu : ((1u << s_count) - 1u);
    return src->mask & valid;
}

const float* stems_source_audio(uint32_t mask, uint64_t* frames,
                                uint32_t* channels, uint32_t* sample_rate)
{
    uint32_t valid = s_count >= 32 ? 0xffffffffu : ((1u << s_count) - 1u);
    mask &= valid;
    if (mask == 0 || !s_audio)
        return audio_pcm_data(s_audio, frames, channels, sample_rate);
    uint32_t ch = 0, sr = 0; uint64_t mix_frames = 0;
    if (!audio_pcm_data(s_audio, &mix_frames, &ch, &sr) || sr == 0)
        return nullptr;
    for (SourceBuf& b : s_sources) {
        if (b.mask != mask) continue;
        if (frames) *frames = b.frames;
        if (channels) *channels = 1;
        if (sample_rate) *sample_rate = sr;
        return b.pcm;
    }
    // First use: decode each stem at the track's rate and sum to mono.
    float* sum = (float*)calloc((size_t)mix_frames, sizeof(float));
    if (!sum) return nullptr;
    for (int i = 0; i < s_count; i++) {
        if (!((mask >> i) & 1u)) continue;
        float* pcm = nullptr; uint64_t fr = 0;
        if (!decode_stem_at(s_stems[i].path, sr, &pcm, &fr)) continue;
        uint64_t n = fr < mix_frames ? fr : mix_frames;
        for (uint64_t k = 0; k < n; k++) sum[k] += 0.5f * (pcm[k * 2] + pcm[k * 2 + 1]);
        free(pcm);
    }
    SourceBuf b; b.mask = mask; b.pcm = sum; b.frames = mix_frames;
    s_sources.push_back(b);
    char lbl[128]; stems_source_label(mask, lbl, sizeof(lbl));
    printf("[stems] analysis source %s: %llu frames\n", lbl, (unsigned long long)mix_frames);
    if (frames) *frames = mix_frames;
    if (channels) *channels = 1;
    if (sample_rate) *sample_rate = sr;
    return sum;
}

void stems_source_label(uint32_t mask, char* out, int out_size) {
    out[0] = 0;
    if (mask == 0) { snprintf(out, out_size, "mix"); return; }
    int len = 0;
    for (int i = 0; i < s_count; i++) {
        if (!((mask >> i) & 1u)) continue;
        len += snprintf(out + len, out_size - len > 0 ? out_size - len : 0, "%s%s",
                        len ? "+" : "", s_stems[i].name);
        if (len >= out_size) break;
    }
}
SpectrogramState* stems_composite() {
    return (s_sel_mask && s_composite.computed) ? &s_composite : nullptr;
}
bool stems_rebuilding() { return s_sel_running.load(); }
