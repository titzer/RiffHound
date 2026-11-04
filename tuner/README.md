# Tuner

A small window that shows the pitch the microphone hears: note name, cents
off, frequency, a cents meter, a scrolling pitch history, and a three-octave
piano that lights the detected note and plays reference tones.  Meant to sit
in a corner while you work; fullscreen makes it readable across the room.

    make            # builds Tuner.app (binary inside the bundle)
    make install    # also links ~/bin/tuner -> Tuner.app/Contents/MacOS/Tuner
    make selftest   # pitch detector on synthetic tones

Needs GLFW (`brew install glfw`); shares beatmapper's vendored Dear ImGui and
miniaudio.  Settings persist in `~/.tuner_settings`.

Keys: Z-row and Q-row play notes (tracker layout, labels on the keys),
Up/Down shift the keyboard octave, Space toggles the mic, F or Cmd+F
fullscreen (Esc leaves), Cmd+, settings, Cmd+W/Q quit.

Detection is YIN (from webui/voice) without the prototype's Hann window,
which read low notes up to 20 cents sharp.

Later: MIDI input and playback, vocal warm-up exercises, a realtime
spectrogram behind the history.
