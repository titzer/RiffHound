#include "player.h"

#include <math.h>
#include <string.h>

bool Player::init() {
    memset(&editor, 0, sizeof editor);
    editor.speed = 1.0f;
    editor.stretch_engine = 0;  // the phase vocoder, beatmapper's default
    audio_init(&audio);
    return true;
}

bool Player::load(const std::string& path) {
    programme.clear();
    current = 0;
    ready = audio_load(&audio, &editor, path.c_str());
    audio_set_loop(&audio, false, 0, 0);
    speed = 1.0f;
    semitones = cents = 0;
    last_pos = 0;
    return ready;
}

void Player::shutdown() { audio_shutdown(&audio); }

void Player::toggle() {
    if (!ready) return;
    if (audio.playing) { audio_pause(&audio); return; }
    if (!looping() && audio.position >= audio.duration - 0.05) seek(0);
    audio_play(&audio);
}

void Player::seek(double t) {
    if (!ready) return;
    if (t < 0) t = 0;
    if (t > audio.duration) t = audio.duration;
    audio_seek(&audio, t);
    quiet = 8;
}

void Player::nudge(double dt) {
    if (!ready) return;
    double t = audio.position + dt;
    if (looping()) {
        // Inside a loop the arrows go round it rather than out of it.
        const LoopEntry& e = programme[current];
        double len = e.to - e.from;
        if (len > 0) t = e.from + fmod(fmod(t - e.from, len) + len, len);
    }
    seek(t);
}

void Player::engage(int entry) {
    current = entry;
    const LoopEntry& e = programme[entry];
    audio_set_loop(&audio, true, e.from, e.to);
    seek(e.from);
}

void Player::add_loop(const Loop& l) {
    if (!ready || l.t1 <= l.t0) return;
    programme.push_back({l.name, l.t0, l.t1});
    if (programme.size() == 1) {
        engage(0);
        if (!audio.playing) audio_play(&audio);
    }
}

void Player::remove_last_loop() {
    if (programme.empty()) return;
    programme.pop_back();
    if (programme.empty()) { clear_loops(); return; }
    if (current >= (int)programme.size()) engage(0);
}

void Player::clear_loops() {
    programme.clear();
    current = 0;
    audio_set_loop(&audio, false, 0, 0);
}

void Player::update() {
    audio_update(&audio);
    double pos = audio.position;
    // The engine wraps a single loop by itself, gaplessly.  Moving on to the
    // next entry of a programme is a seek made here once the wrap is seen,
    // so a programme of several loops has a hiccup of up to a frame at each
    // change.  Gapless would need a "jump to" in the stretch source, which
    // fits with the speed/pitch work.
    //
    // A wrap is the position going backwards to near the loop's start with
    // no seek of ours in the last few frames (the engine applies a seek late,
    // so the cursor can report the old place once or twice after one).
    if (looping() && audio.playing && quiet == 0 && programme.size() > 1 &&
        pos < last_pos - 0.05 && pos < programme[current].from + 0.5) {
        int next = (current + 1) % (int)programme.size();
        const LoopEntry &a = programme[current], &b = programme[next];
        if (a.from == b.from && a.to == b.to) current = next;
        else engage(next);
        pos = audio.position;
    }
    if (quiet > 0) quiet--;
    last_pos = pos;
}

// --- tuning (stub) ---------------------------------------------------------
//
// The controls exist and keep their values so the screen and keys are final;
// apply_tuning is where they will reach the engine:
//     audio_set_speed(&editor, speed);
//     audio_set_pitch(&editor, semitones, cents);

void Player::apply_tuning() {}

void Player::set_speed(float s) {
    s = roundf(s * 20) / 20;
    if (s < 0.5f) s = 0.5f;
    if (s > 1.5f) s = 1.5f;
    speed = s;
    apply_tuning();
}

void Player::transpose(int ds, int dc) {
    semitones += ds;
    cents += dc;
    if (cents > 100) cents = 100;
    if (cents < -100) cents = -100;
    if (semitones > 12) semitones = 12;
    if (semitones < -12) semitones = -12;
    apply_tuning();
}

void Player::reset_tuning() {
    semitones = cents = 0;
    apply_tuning();
}
