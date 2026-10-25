#ifndef __STRATEGY_H
#define __STRATEGY_H

#include "common.h"
#include "bidiarray.h"
#include "move.h"

class Strategy {
private:
    bidiarray<Sint16> _blobs;
    const bidiarray<bool>& _holes;
    Uint16 _current_player;
    void (*_saveBestMove)(movement&);

public:
    Strategy (bidiarray<Sint16>& blobs,
              const bidiarray<bool>& holes,
              const Uint16 current_player,
              void (*saveBestMove)(movement&))
        : _blobs(blobs), _holes(holes), _current_player(current_player), _saveBestMove(saveBestMove)
    {}

    Strategy (const Strategy& St)
        : _blobs(St._blobs), _holes(St._holes), _current_player(St._current_player)
    {}

    ~Strategy() {}

    void applyMove (const movement& mv);
    vector<movement>& computeValidMoves (vector<movement>& valid_moves) const;
    Sint32 estimateCurrentScore () const;
    void computeBestMove ();

    // Compatibility declarations required by strategies/strategy_minimax.cc.
    // The historical blobmap Strategy API did not contain these declarations.
    Sint32 minimax(int depth);
    bool isEndGame() const;
};

#endif
