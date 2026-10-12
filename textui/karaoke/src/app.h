#pragma once
// The karaoke screen and the load dialog: state, drawing, keys.

#include <string>
#include <vector>

#include "chart.h"
#include "library.h"
#include "player.h"
#include "term.h"

enum Filter { F_LYRICS, F_CHORDS, F_BEATS, F_LOOPS, F_MELODY, F_TRADE, N_FILTERS };

struct PickRow {
    int track = -1;     // a track row
    int list = -1;      // a list heading row
};

struct Picker {
    bool open = false;
    bool lists_tab = false;
    int  open_list = -1;            // the list being browsed, in the lists tab
    std::string query;
    bool filters[N_FILTERS] = {};
    std::vector<PickRow> rows;
    int  cursor = 0, scroll = 0;
    std::string cursor_key;         // what the cursor is on, kept across refilters
};

struct App {
    Library library;
    Player  player;
    bool    audio = true;           // false in --dump: charts only, no sound

    int     track = -1;             // index into library.tracks
    Chart   chart;
    bool    have_chart = false;
    std::string load_error;

    Picker  picker;
    std::string message;            // a transient line in the footer
    double  message_until = 0;
    bool    quit = false;

    // In --dump the clock is this rather than the player's.
    double  fake_time = 0;

    bool   open_track(int index, double start_at, bool play);
    double now() const;
    double duration() const;
    bool   playing() const;

    void   draw(Screen& s);
    void   key(int k);

    void   open_picker();
    void   refilter();

private:
    void draw_main(Screen& s);
    void draw_picker(Screen& s);
    int  draw_lyrics(Screen& s, int top, int rows);
    void draw_chords(Screen& s, int top);
    void draw_header(Screen& s);
    void draw_footer(Screen& s);
    void play_key(int k);
    void picker_key(int k);
    void say(const std::string& m);
    bool passes(const Track& t) const;
};

double wall_clock();
