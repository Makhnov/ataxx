#include "strategy.h"



void Strategy::applyMove (const movement& mv) {
    // changer la case d'arrivée
    _blobs.set(mv.nx, mv.ny, _current_player);

    // si c'est un saut, vider la case de départ
    int dx = abs(mv.nx - mv.ox);
    int dy = abs(mv.ny - mv.oy);
    if(std::max(dx,dy) == 2){
        _blobs.set(mv.ox, mv.oy, -1);
    }

    // convertir les blobs adverses entourant la case d'arrivée
    for(int x = std::max(0, mv.nx-1); x <= std::min(7, mv.nx+1); x++){
        for(int y = std::max(0, mv.ny-1); y <= std::min(7, mv.ny+1); y++){
            Sint16 val = _blobs.get(x, y);
            if(val != -1 && val != (Sint16)_current_player){
                _blobs.set(x, y, _current_player);
            }
        }
    }

    // changer de joueur
    _current_player = 1 - _current_player;
}

Sint32 Strategy::estimateCurrentScore () const {
    Sint32 my_blobs = 0, opp_blobs = 0;
    for(int x = 0; x < 8; x++){
        for(int y = 0; y < 8; y++){
            Sint16 val = _blobs.get(x, y);
            if(val == (Sint16)_current_player) my_blobs++;
            else if(val != -1) opp_blobs++;
        }
    }
    if(opp_blobs == 0) return 1000;
    if(my_blobs == 0) return -1000;
    return my_blobs - opp_blobs;
}

vector<movement>& Strategy::computeValidMoves (vector<movement>& valid_moves) const {
    movement mv(0,0,0,0);

    for(mv.ox = 0; mv.ox < 8; mv.ox++){
        for(mv.oy = 0; mv.oy < 8; mv.oy++){
            if(_blobs.get(mv.ox, mv.oy) == (Sint16) _current_player){

                for(mv.nx = std::max(0, mv.ox-2); mv.nx <= std::min(7, mv.ox+2); mv.nx++){
                    for(mv.ny = std::max(0, mv.oy-2); mv.ny <= std::min(7, mv.oy+2); mv.ny++){

                        // ignorer si la nouvelle position est un trou
                        if(_holes.get(mv.nx, mv.ny)) continue;
                        
                        //  ignorer si non vide
                        if (mv.nx == mv.ox && mv.ny == mv.oy) continue;

                        // ajouter si vide
                        if(_blobs.get(mv.nx, mv.ny) == -1){
                            valid_moves.push_back(mv);
                        }
                    }
                }
            }
        }
    }
    return valid_moves;
}

void Strategy::computeBestMove () {
    vector<movement> valid_moves;
    computeValidMoves(valid_moves);

    std::cout<< valid_moves.size()<<std::endl;

    movement best_move(0,0,0,0);
    Sint32 best_score = -10000;

    for(const auto& mv: valid_moves){

        Strategy next(*this);

        next.applyMove(mv);

        Sint32 score = next.estimateCurrentScore();
        
        if(score > best_score){
            best_score = score;
            best_move = mv;
        }
    }

    goto end_choice;

end_choice:
    _saveBestMove(best_move);
    return;
}
