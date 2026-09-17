#pragma once

// The application icon, embedded so it is available however the binary is
// launched.  Straight (unpremultiplied) RGBA, row-major, top row first.
// Regenerate with `make icon` (scripts/make-icon.py) after changing icon.png.
extern const int ICON_W;
extern const int ICON_H;
extern const unsigned char ICON_RGBA[];
