#include "strategy.h"

static const Sint32 Infinity = 10000;

static void applyMove(bidiarray<Sint16>& blobs,
                      Uint16& current_player,
                      const movement& mv) {
    // changer la case d'arrivee
    blobs.set(mv.nx, mv.ny, current_player);

    // si c'est un saut, vider la case de depart
    int dx = abs(mv.nx - mv.ox);
    int dy = abs(mv.ny - mv.oy);
    if (std::max(dx, dy) == 2) {
        blobs.set(mv.ox, mv.oy, -1);
    }

    // convertir les blobs adverses entourant la case d'arrivee
    for (int x = std::max(0, mv.nx - 1); x <= std::min(7, mv.nx + 1); x++) {
        for (int y = std::max(0, mv.ny - 1); y <= std::min(7, mv.ny + 1); y++) {
            Sint16 val = blobs.get(x, y);
            if (val != -1 && val != (Sint16)current_player) {
                blobs.set(x, y, current_player);
            }
        }
    }

    // changer de joueur
    current_player = 1 - current_player;
}

static vector<movement>& computeValidMoves(const bidiarray<Sint16>& blobs,
                                           const bidiarray<bool>& holes,
                                           Uint16 current_player,
                                           vector<movement>& valid_moves) {
    movement mv(0, 0, 0, 0);

    for (mv.ox = 0; mv.ox < 8; mv.ox++) {
        for (mv.oy = 0; mv.oy < 8; mv.oy++) {
            if (blobs.get(mv.ox, mv.oy) == (Sint16)current_player) {
                for (mv.nx = std::max(0, mv.ox - 2); mv.nx <= std::min(7, mv.ox + 2); mv.nx++) {
                    for (mv.ny = std::max(0, mv.oy - 2); mv.ny <= std::min(7, mv.oy + 2); mv.ny++) {
                        // ignorer si la nouvelle position est un trou
                        if (holes.get(mv.nx, mv.ny)) continue;

                        // ignorer si meme case
                        if (mv.nx == mv.ox && mv.ny == mv.oy) continue;

                        // ajouter si vide
                        if (blobs.get(mv.nx, mv.ny) == -1) {
                            valid_moves.push_back(mv);
                        }
                    }
                }
            }
        }
    }

    return valid_moves;
}

static Sint32 estimateCurrentScore(const bidiarray<Sint16>& blobs,
                                   Uint16 current_player) {
    Sint32 my_blobs = 0;
    Sint32 opp_blobs = 0;

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            Sint16 val = blobs.get(x, y);
            if (val == (Sint16)current_player) my_blobs++;
            else if (val != -1) opp_blobs++;
        }
    }

    /*
     * if (opp_blobs == 0) return 1000;
     * if (my_blobs == 0) return -1000;
     */
    return my_blobs - opp_blobs;
}

static bool playerHasBlob(const bidiarray<Sint16>& blobs, Uint16 player) {
    for (Uint8 x = 0; x < 8; x++) {
        for (Uint8 y = 0; y < 8; y++) {
            if (blobs.get(x, y) == (Sint16)player) {
                return true;
            }
        }
    }
    return false;
}

static bool playerCanMove(const bidiarray<Sint16>& blobs,
                          const bidiarray<bool>& holes,
                          Uint16 player) {
    for (Sint16 x = 0; x < 8; x++) {
        for (Sint16 y = 0; y < 8; y++) {
            if (holes.get(x, y) || blobs.get(x, y) != -1) continue;

            for (Sint16 dx = -2; dx <= 2; dx++) {
                for (Sint16 dy = -2; dy <= 2; dy++) {
                    Sint16 nx = x + dx;
                    Sint16 ny = y + dy;

                    if (nx < 0 || ny < 0 || nx > 7 || ny > 7) continue;
                    if (blobs.get(nx, ny) == (Sint16)player) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool isEndGame(const bidiarray<Sint16>& blobs,
                      const bidiarray<bool>& holes) {
    if (!playerHasBlob(blobs, 0) || !playerHasBlob(blobs, 1)) {
        return true;
    }

    return !playerCanMove(blobs, holes, 0) && !playerCanMove(blobs, holes, 1);
}

static Sint32 minimax(const bidiarray<Sint16>& blobs,
                      const bidiarray<bool>& holes,
                      Uint16 current_player,
                      int depth) {
    if (depth == 0 || isEndGame(blobs, holes)) {
        return estimateCurrentScore(blobs, current_player);
    }

    vector<movement> valid_moves;
    computeValidMoves(blobs, holes, current_player, valid_moves);

    if (valid_moves.empty()) {
        return -minimax(blobs, holes, 1 - current_player, depth - 1);
    }

    Sint32 best_score = -Infinity;

    for (const movement& mv : valid_moves) {
        bidiarray<Sint16> next_blobs(blobs);
        Uint16 next_player = current_player;
        applyMove(next_blobs, next_player, mv);

        Sint32 score = -minimax(next_blobs, holes, next_player, depth - 1);
        if (score > best_score) {
            best_score = score;
        }
    }

    return best_score;
}

void Strategy::applyMove (const movement& mv) {
    ::applyMove(_blobs, _current_player, mv);
}

Sint32 Strategy::estimateCurrentScore () const {
    return ::estimateCurrentScore(_blobs, _current_player);
}

vector<movement>& Strategy::computeValidMoves (vector<movement>& valid_moves) const {
    return ::computeValidMoves(_blobs, _holes, _current_player, valid_moves);
}

void Strategy::computeBestMove () {
    vector<movement> valid_moves;
    computeValidMoves(valid_moves);

    if (valid_moves.empty()) {
        return;
    }

    // Sauvegarder immediatement un coup valide au cas ou le timer nous tue
    movement safe_move = valid_moves[rand() % valid_moves.size()];
    _saveBestMove(safe_move);

    movement best_move = valid_moves[0];
    const int move_count = static_cast<int>(valid_moves.size());
    const int max_depth = 20;

    for (int depth = 1; depth <= max_depth; ++depth) {
        vector<Sint32> scores(move_count, -Infinity);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
        for (int i = 0; i < move_count; ++i) {
            bidiarray<Sint16> next_blobs(_blobs);
            Uint16 next_player = _current_player;
            ::applyMove(next_blobs, next_player, valid_moves[i]);
            scores[i] = -minimax(next_blobs, _holes, next_player, depth);
        }

        int depth_best_index = 0;
        Sint32 depth_best_score = scores[0];

        for (int i = 1; i < move_count; ++i) {
            if (scores[i] > depth_best_score) {
                depth_best_score = scores[i];
                depth_best_index = i;
            }
        }

        best_move = valid_moves[depth_best_index];
        _saveBestMove(best_move);

#ifdef DEBUG
        fprintf(stderr, "[MINIMAX_PAR] depth=%d | score=%d | best=(%d,%d)->(%d,%d)\n",
                depth, depth_best_score,
                best_move.ox, best_move.oy, best_move.nx, best_move.ny);
#endif
    }
}
