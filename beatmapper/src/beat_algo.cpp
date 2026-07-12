#include "beat_algo.h"
#include <string.h>

void autobeat_init(AutoBeatList* ab) {
    memset(ab, 0, sizeof(*ab));
}

const BeatAlgoDesc BEAT_ALGOS[] = {
    {
        "Spectral Flux + Ellis DP",
        "Spectral flux onset detection → autocorrelation tempo → Ellis DP beat tracking",
        beat_spectral_flux
    },
};

const int BEAT_ALGO_COUNT = 1;
