#include "strategy.h"
#include <chrono>

namespace {
using SearchClock = std::chrono::steady_clock;

SearchClock::time_point search_deadline;
bool search_timeout = false;

bool timeIsUp() {
    if (SearchClock::now() >= search_deadline) {
        search_timeout = true;
        return true;
    }
    return false;
}
} // namespace

void Strategy::applyMove(const movement &mv) {
    // changer la case d'arrivée
    _blobs.set(mv.nx, mv.ny, _current_player);

    // si c'est un saut, vider la case de départ
    int dx = abs(mv.nx - mv.ox);
    int dy = abs(mv.ny - mv.oy);
    if (std::max(dx, dy) == 2) {
        _blobs.set(mv.ox, mv.oy, -1);
    }

    // convertir les blobs adverses entourant la case d'arrivée
    for (int x = std::max(0, mv.nx - 1); x <= std::min(7, mv.nx + 1); x++) {
        for (int y = std::max(0, mv.ny - 1); y <= std::min(7, mv.ny + 1); y++) {
            Sint16 val = _blobs.get(x, y);
            if (val != -1 && val != (Sint16)_current_player) {
                _blobs.set(x, y, _current_player);
            }
        }
    }

    // changer de joueur
    _current_player = 1 - _current_player;
}

Sint32 Strategy::estimateCurrentScore() const {
    Sint32 my_blobs = 0, opp_blobs = 0;
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            Sint16 val = _blobs.get(x, y);
            if (val == (Sint16)_current_player)
                my_blobs++;
            else if (val != -1)
                opp_blobs++;
        }
    }
    if (opp_blobs == 0)
        return 1000;
    if (my_blobs == 0)
        return -1000;
    return my_blobs - opp_blobs;
}

vector<movement> &Strategy::computeValidMoves(vector<movement> &valid_moves) const {
    movement mv(0, 0, 0, 0);

    for (mv.ox = 0; mv.ox < 8; mv.ox++) {
        for (mv.oy = 0; mv.oy < 8; mv.oy++) {
            if (_blobs.get(mv.ox, mv.oy) == (Sint16)_current_player) {

                for (mv.nx = std::max(0, mv.ox - 2); mv.nx <= std::min(7, mv.ox + 2); mv.nx++) {
                    for (mv.ny = std::max(0, mv.oy - 2); mv.ny <= std::min(7, mv.oy + 2); mv.ny++) {

                        // ignorer si la nouvelle position est un trou
                        if (_holes.get(mv.nx, mv.ny))
                            continue;

                        //  ignorer si non vide
                        if (mv.nx == mv.ox && mv.ny == mv.oy)
                            continue;

                        // ajouter si vide
                        if (_blobs.get(mv.nx, mv.ny) == -1) {
                            valid_moves.push_back(mv);
                        }
                    }
                }
            }
        }
    }
    return valid_moves;
}

void Strategy::computeBestMove() {

    vector<movement> valid_moves;
    computeValidMoves(valid_moves);

    if (valid_moves.empty()) {
        return;
    }

    movement best_move = valid_moves[0];
    bool published = false;

    search_timeout = false;
    search_deadline =
        SearchClock::now() + std::chrono::milliseconds(900);

    // Iterative deepening
    for (int depth = 1; depth <= 4; depth++) {

        movement depth_best_move = best_move;
        Sint32 depth_best_score = -10000;
        bool depth_completed = true;

        for (const auto &mv : valid_moves) {

            if (timeIsUp()) {
                depth_completed = false;
                break;
            }

            Strategy next(*this);
            next.applyMove(mv);

            Sint32 score = -next.minimax(depth);

            if (search_timeout) {
                depth_completed = false;
                break;
            }

            if (score > depth_best_score) {
                depth_best_score = score;
                depth_best_move = mv;
            }
        }

        if (!depth_completed) {
            break;
        }

        best_move = depth_best_move;
        _saveBestMove(best_move);
        published = true;
    }

    // toujours fournir un coup légal même dans en cas de tps écoulé
    if (!published) {
        _saveBestMove(best_move);
    }
}

Sint32 Strategy::minimax(int depth) {

    if (timeIsUp()) {
        return 0;
    }

    if (depth == 0 || isEndGame()) {
        return estimateCurrentScore();
    }

    vector<movement> valid_moves;
    computeValidMoves(valid_moves);

    Sint32 best_score = -10000;

    for (const auto &mv : valid_moves) {

        if (timeIsUp()) {
            return 0;
        }

        Strategy next(*this);
        next.applyMove(mv);

        Sint32 score = -next.minimax(depth - 1);

        if (search_timeout) {
            return 0;
        }

        if (score > best_score) {
            best_score = score;
        }
    }

    return best_score;
}