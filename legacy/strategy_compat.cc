#include "strategy.h"

bool Strategy::isEndGame() const {
    bool has_blob[2] = {false, false};

    for (Uint8 x = 0; x < 8; ++x) {
        for (Uint8 y = 0; y < 8; ++y) {
            Sint16 value = _blobs.get(x, y);
            if (value == 0 || value == 1) {
                has_blob[value] = true;
            }
        }
    }

    if (!has_blob[0] || !has_blob[1]) {
        return true;
    }

    bool can_move[2] = {false, false};

    for (Sint16 x = 0; x < 8; ++x) {
        for (Sint16 y = 0; y < 8; ++y) {
            if (_holes.get(x, y) || _blobs.get(x, y) != -1) {
                continue;
            }

            for (Sint16 dx = -2; dx <= 2; ++dx) {
                for (Sint16 dy = -2; dy <= 2; ++dy) {
                    Sint16 nx = x + dx;
                    Sint16 ny = y + dy;

                    if (nx < 0 || ny < 0 || nx > 7 || ny > 7) {
                        continue;
                    }

                    Sint16 value = _blobs.get(nx, ny);
                    if (value == 0 || value == 1) {
                        can_move[value] = true;
                    }
                }
            }
        }
    }

    return !can_move[0] && !can_move[1];
}
