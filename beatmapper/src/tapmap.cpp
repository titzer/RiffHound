#include "tapmap.h"

bool tapmap_add(TapMap* tm, double t, bool selected) {
    if (tm->count >= TAP_MAX) return false;
    tm->taps[tm->count++] = { t, selected };
    return true;
}

void tapmap_remove(TapMap* tm, int idx) {
    if (idx < 0 || idx >= tm->count) return;
    for (int i = idx; i + 1 < tm->count; i++) tm->taps[i] = tm->taps[i + 1];
    tm->count--;
}

void tapmap_sort(TapMap* tm) {
    for (int a = 1; a < tm->count; a++)
        for (int b = a; b > 0 && tm->taps[b].time < tm->taps[b - 1].time; b--) {
            TapEntry tmp = tm->taps[b]; tm->taps[b] = tm->taps[b - 1]; tm->taps[b - 1] = tmp;
        }
}

void tapmap_clear(TapMap* tm) {
    tm->count = 0;
}

bool tapmap_any_selected(const TapMap* tm) {
    for (int i = 0; i < tm->count; i++)
        if (tm->taps[i].selected) return true;
    return false;
}
