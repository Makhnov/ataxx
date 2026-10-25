#include "strategy.h"

#include <algorithm>
#include <cstdlib>

typedef Uint64 Bitboard;

enum Player {
    RED = 0,
    BLUE = 1
};

struct Board {
    Bitboard red = 0;
    Bitboard blue = 0;
    Bitboard holes = 0;
    Uint16 turn = RED;
};

struct Move {
    int from;
    int to;
};

// Initialisation (Tableaux de mouvements, etc.)
static bool init = false;

// Helpers de calcul rapides pour les mouvements valides de clonage (cases adjacentes) en fonction de la carte
static Bitboard validClones[64];

// Pareil pour les sauts (cases à distance 2)
static Bitboard validJumps[64];

static const Sint32 INF = 1000000;
static int SEARCH_DEPTH = 1;

// Renvoie l'indexage (au format uint64) de la case de coordonnées (x, y)
static inline int index(int x, int y) {
    return y * 8 + x;
}

// Renvoie un board 'masque' avec un 1 à la position de la case d'index i
static inline Bitboard mask(int s) {
    return ((Uint64)1 << s);
}

// Renvoie l'abscisse de la case d'index i
static inline int X(int i) {
    return i & 7;
}

// Renvoie l'ordonnée de la case d'index i
static inline int Y(int i) {
    return i >> 3;
}

// Renvoie l'index de la première case à 1 sur un plateau de jeu (bit le plus à droite en comptant le nombre de 0)
static inline int next(Bitboard bb) {
    return __builtin_ctzll((unsigned long long)bb);
}

// On crée un plateau qui contient toutes les cases occupées (pions rouges, bleus et trous)
static inline Bitboard denied(const Board &b) {
    return b.red | b.blue | b.holes;
}

// On crée le plateau inverse (toutes les cases libres)
static inline Bitboard allowed(const Board &b) {
    return ~denied(b);
}

// Petit helper pour rendre le code plus lisible (alternance des joueurs)
static inline Uint16 opponent(Uint16 p) {
    return (p == 0) ? 1 : 0;
}

// Compte le nombre de pions sur un bitboard (nb de 1 dans un long long)
static inline int blobs(Bitboard bb) {
    return __builtin_popcountll((unsigned long long)bb);
}

// Création de notre plateau de travail (bitboards) à partir de la situation actuelle du jeu
static inline Board makeBoard(const bidiarray<Sint16> &blobs, const bidiarray<bool> &holes) {
    Board b;

    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            Bitboard m = mask(index(x, y));

            if (holes.get(x, y)) {
                b.holes |= m;
            } else if (blobs.get(x, y) == 0) {
                b.red |= m;
            } else if (blobs.get(x, y) == 1) {
                b.blue |= m;
            }
        }
    }

    return b;
}

// Initialisation des helpers de vérification de mouvements valides
static void initValid() {
    for (int from = 0; from < 64; ++from) {
        int fx = X(from);
        int fy = Y(from);

        validClones[from] = 0;
        validJumps[from] = 0;

        for (int x = std::max(0, fx - 2); x <= std::min(7, fx + 2); ++x) {
            for (int y = std::max(0, fy - 2); y <= std::min(7, fy + 2); ++y) {
                if (x == fx && y == fy)
                    continue;

                int dx = std::abs(x - fx);
                int dy = std::abs(y - fy);
                int d = std::max(dx, dy);
                int to = index(x, y);

                if (d == 1) {
                    validClones[from] |= mask(to);
                } else if (d == 2) {
                    validJumps[from] |= mask(to);
                }
            }
        }
    }

    init = true;
}

// Micro factory pour assurer qu'on à bien fait nos initialisations avant de lancer le calcul du meilleur coup
static inline void initialize() {
    if (init) {
        return;
    }

    // Tableaux de mouvements valides
    initValid();

    // Initialisation effectuée
    init = true;
}

// Actualisation du plateau après la validation d'un mouvement (clonage ou saut)
static inline Board applyMove(const Board &b, const Move &mv) {
    Board child = b;

    Uint16 player = b.turn;
    Bitboard &me = (player == RED) ? child.red : child.blue;
    Bitboard &opp = (player == RED) ? child.blue : child.red;

    Bitboard fromMask = mask(mv.from);
    Bitboard toMask = mask(mv.to);

    if (validJumps[mv.from] & toMask) {
        me &= ~fromMask;
    }

    me |= toMask;

    Bitboard flips = validClones[mv.to] & opp;
    opp &= ~flips;
    me |= flips;

    child.turn = opponent(player);
    return child;
}

// Vérifie si un joueur peut encore jouer
static inline bool playerCanMove(const Board &b, Uint16 player) {
    Bitboard bb = (player == RED) ? b.red : b.blue;
    Bitboard legals = allowed(b);

    while (bb) {
        int from = next(bb);
        bb &= (bb - 1);

        if (validClones[from] & legals)
            return true;
        if (validJumps[from] & legals)
            return true;
    }

    return false;
}

/* STRATEGIES */

/* ALPHA - BETA */

// Heuristique d'évaluation de la position
static inline Sint32 evaluate(const Board &b) {
    Sint32 score = blobs(b.red) - blobs(b.blue);

    if (b.turn == RED)
        return score;
    return -score;
}

// Signature de la fonction wrapper (qui apelle negamax)
movement alphabeta(const Board &b, Bitboard bb);

// Fonction de recherche récursive (negamax avec élagage alpha-beta)
static Sint32 negamax(const Board &b, int depth, Sint32 alpha, Sint32 beta) {
    if (depth == 0) {
        return evaluate(b);
    }

    Uint16 toMove = b.turn;
    Bitboard bb = (toMove == RED) ? b.red : b.blue;
    Bitboard legals = allowed(b);

    bool foundMove = false;
    Sint32 best = -INF;

    for (Bitboard it = bb; it != 0; it &= (it - 1)) {
        int from = next(it);

        for (Bitboard clones = validClones[from] & legals; clones != 0; clones &= (clones - 1)) {
            int to = next(clones);
            Move mv = {from, to};
            Board child = ::applyMove(b, mv);

            Sint32 score = -negamax(child, depth - 1, -beta, -alpha);
            foundMove = true;

            if (score > best)
                best = score;
            if (score > alpha)
                alpha = score;
            if (alpha >= beta)
                return best;
        }

        for (Bitboard jumps = validJumps[from] & legals; jumps != 0; jumps &= (jumps - 1)) {
            int to = next(jumps);
            Move mv = {from, to};
            Board child = ::applyMove(b, mv);

            Sint32 score = -negamax(child, depth - 1, -beta, -alpha);
            foundMove = true;

            if (score > best)
                best = score;
            if (score > alpha)
                alpha = score;
            if (alpha >= beta)
                return best;
        }
    }

    // Si on n'a pas trouvé de coup, on vérifie si l'adversaire peut jouer, sinon on évalue la position
    if (!foundMove) {
        if (!playerCanMove(b, opponent(toMove))) {
            return evaluate(b);
        }

        Board child = b;
        child.turn = opponent(toMove);
        return -negamax(child, depth - 1, -beta, -alpha);
    }

    return best;
}

void Strategy::applyMove(const movement &mv) {
    // To be completed...
}

Sint32 Strategy::estimateCurrentScore() const {
    // To be completed...
    return 0;
}

vector<movement> &Strategy::computeValidMoves(vector<movement> &valid_moves) const {
    // To be completed...
    return valid_moves;
}

void Strategy::computeBestMove() {
    // To be improved...

    // Initialisations des helpers de calcul rapide
    initialize();

    // Transformation de nos données en bitboards
    Board b = makeBoard(_blobs, _holes);

    // Joueur courant
    b.turn = _current_player;

    // On récupère les pions du joueur courant
    Bitboard bb = (b.turn == RED) ? b.red : b.blue;

    // On lance une recherche alpha-beta pour trouver le meilleur coup
    for (int depth = 1;; ++depth) {
        SEARCH_DEPTH = depth;

        movement mv = ::alphabeta(b, bb);

        if (!(mv.ox == mv.nx && mv.oy == mv.ny)) {
            std::cerr << "[BLUE] depth=" << depth << std::endl;
            _saveBestMove(mv);
        }
    }

    return;
}

movement alphabeta(const Board &b, Bitboard bb) {
    Bitboard legals = allowed(b);

    // On cherche le meilleur coup
    Move best;

    Sint32 bestScore = -INF;

    for (best = {-1, -1}; bb != 0; bb &= (bb - 1)) {
        int from = next(bb);

        for (Bitboard clones = validClones[from] & legals; clones != 0; clones &= (clones - 1)) {
            int to = next(clones);
            Move mv = {from, to};
            Board child = ::applyMove(b, mv);

            Sint32 score = -negamax(child, SEARCH_DEPTH - 1, -INF, INF);

            if (best.from == -1 || score > bestScore) {
                best.from = from;
                best.to = to;
                bestScore = score;
            }
        }

        for (Bitboard jumps = validJumps[from] & legals; jumps != 0; jumps &= (jumps - 1)) {
            int to = next(jumps);
            Move mv = {from, to};
            Board child = ::applyMove(b, mv);

            Sint32 score = -negamax(child, SEARCH_DEPTH - 1, -INF, INF);

            if (best.from == -1 || score > bestScore) {
                best.from = from;
                best.to = to;
                bestScore = score;
            }
        }
    }

    // Si on n'a pas trouvé de coup, on retourne un mouvement invalide
    if (best.from == -1) {
        return movement(0, 0, 0, 0);
    }

    movement mv(X(best.from), Y(best.from), X(best.to), Y(best.to));
    return mv;
}