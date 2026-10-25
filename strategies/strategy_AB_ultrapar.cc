#include "strategy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

typedef Uint64 Bitboard;
enum Player { RED = 0,
              BLUE = 1 };

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

struct RankedMove {
    Move mv;
    Sint32 score;
};
// Table de transposition

enum TTFlag { TT_EMPTY = 0,
              TT_EXACT = 1,
              TT_LOWER = 2,
              TT_UPPER = 3 };

struct TTEntry {
    Uint64 key = 0;
    Sint32 value = 0;
    int depth = -1;
    int flag = TT_EMPTY;
    Move best = {-1, -1};
};

static bool init_done = false;
static const Sint32 INF = 1000000;
static const int NUM_THREADS = 12;

static Bitboard validClones[64];
static Bitboard validJumps[64];

// TT partagée entre tous les threads (coeur du Lazy SMP)
static const std::size_t TT_SIZE = 1u << 20; // 1M entrées — 2x blue
static TTEntry transTable[TT_SIZE];

// Zobrist Hashing
static Uint64 zobristPiece[2][64];
static Uint64 zobristTurn;
static bool zobrist_init = false;

// Stats (non-atomique, approximatif suffit pour les logs)
static uint64_t NODES = 0;

static inline int index(int x, int y) { return y * 8 + x; }
static inline Bitboard mask(int s) { return ((Uint64)1 << s); }
static inline int X(int i) { return i & 7; }
static inline int Y(int i) { return i >> 3; }
static inline int next(Bitboard bb) { return __builtin_ctzll((unsigned long long)bb); }
static inline int popcount(Bitboard bb) { return __builtin_popcountll((unsigned long long)bb); }
static inline Bitboard denied(const Board &b) { return b.red | b.blue | b.holes; }
static inline Bitboard allowed(const Board &b) { return ~denied(b); }
static inline Uint16 opponent(Uint16 p) { return p ^ 1; }
static inline const char *playerName(Uint16 p) { return (p == RED) ? "RED" : "BLUE"; }

static inline Move invalidMove() { return {-1, -1}; }
static inline bool hasMove(const Move &mv) { return mv.from >= 0 && mv.to >= 0; }
static inline bool sameMove(const Move &a, const Move &b) { return a.from == b.from && a.to == b.to; }
// Hachage Zobrist

static void initZobrist() {
    if (zobrist_init)
        return;
    Uint64 state = 0xDEADBEEFCAFEBABEULL;
    auto splitmix = [&]() -> Uint64 {
        state += 0x9e3779b97f4a7c15ULL;
        Uint64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };
    for (int p = 0; p < 2; ++p)
        for (int s = 0; s < 64; ++s)
            zobristPiece[p][s] = splitmix();
    zobristTurn = splitmix();
    zobrist_init = true;
}

static inline Uint64 hashBoard(const Board &b) {
    Uint64 h = 0;
    for (Bitboard bb = b.red; bb; bb &= (bb - 1))
        h ^= zobristPiece[RED][next(bb)];
    for (Bitboard bb = b.blue; bb; bb &= (bb - 1))
        h ^= zobristPiece[BLUE][next(bb)];
    if (b.turn == BLUE)
        h ^= zobristTurn;
    return h;
}

// Hash incrémental après un move
static inline Uint64 hashAfterMove(Uint64 h, const Board &parent, const Move &mv) {
    Uint16 player = parent.turn;
    Uint16 opp = opponent(player);
    Bitboard toMask = mask(mv.to);

    if (validJumps[mv.from] & toMask)
        h ^= zobristPiece[player][mv.from];
    h ^= zobristPiece[player][mv.to];

    Bitboard oppBB = (player == RED) ? parent.blue : parent.red;
    for (Bitboard f = validClones[mv.to] & oppBB; f; f &= (f - 1)) {
        int sq = next(f);
        h ^= zobristPiece[opp][sq];
        h ^= zobristPiece[player][sq];
    }
    h ^= zobristTurn;
    return h;
}
// Table de transposition

static inline TTEntry *probeTT(Uint64 key) {
    TTEntry &e = transTable[key & (TT_SIZE - 1)];
    if (e.flag != TT_EMPTY && e.key == key)
        return &e;
    return nullptr;
}

static inline void storeTT(Uint64 key, int depth, Sint32 value, int flag, const Move &best) {
    TTEntry &e = transTable[key & (TT_SIZE - 1)];
    // Replace-by-depth : on garde l'info la plus profonde
    if (e.flag == TT_EMPTY || e.key == key || depth >= e.depth) {
        e.key = key;
        e.depth = depth;
        e.value = value;
        e.flag = flag;
        e.best = best;
    }
}
// Manipulation du plateau

static inline Board makeBoard(const bidiarray<Sint16> &blobs_arr, const bidiarray<bool> &holes) {
    Board b;
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            Bitboard m = mask(index(x, y));
            if (holes.get(x, y))
                b.holes |= m;
            else if (blobs_arr.get(x, y) == 0)
                b.red |= m;
            else if (blobs_arr.get(x, y) == 1)
                b.blue |= m;
        }
    return b;
}

static inline Board applyMove(const Board &b, const Move &mv) {
    Board child = b;
    Bitboard &me = (b.turn == RED) ? child.red : child.blue;
    Bitboard &opp = (b.turn == RED) ? child.blue : child.red;
    Bitboard toMask = mask(mv.to);
    if (validJumps[mv.from] & toMask)
        me &= ~mask(mv.from);
    me |= toMask;
    Bitboard flips = validClones[mv.to] & opp;
    opp &= ~flips;
    me |= flips;
    child.turn = opponent(b.turn);
    return child;
}

static inline bool playerCanMove(const Board &b, Uint16 player) {
    Bitboard bb = (player == RED) ? b.red : b.blue;
    Bitboard legals = allowed(b);
    while (bb) {
        int from = next(bb);
        bb &= (bb - 1);
        if ((validClones[from] | validJumps[from]) & legals)
            return true;
    }
    return false;
}

static inline bool isLegalMove(const Board &b, Uint16 player, const Move &mv) {
    if (!hasMove(mv))
        return false;
    Bitboard me = (player == RED) ? b.red : b.blue;
    if (!(me & mask(mv.from)))
        return false;
    if (denied(b) & mask(mv.to))
        return false;
    return ((validClones[mv.from] | validJumps[mv.from]) & mask(mv.to)) != 0;
}
// Evaluation de position

static inline Sint32 fatality(const Board &b, Uint16 player) {
    Sint32 me = popcount((player == RED) ? b.red : b.blue);
    Sint32 opp = popcount((player == RED) ? b.blue : b.red);
    Sint32 margin = me - opp;
    if (margin > 0)
        return 50000 + 100 * margin;
    if (margin < 0)
        return -50000 + 100 * margin;
    return 0;
}

static inline Sint32 evaluate(const Board &b, Uint16 player) {
    Sint32 me = popcount((player == RED) ? b.red : b.blue);
    Sint32 opp = popcount((player == RED) ? b.blue : b.red);
    if (opp == 0)
        return 10000;
    if (me == 0)
        return -10000;
    return me - opp;
}
/*
 * Recherche negamax
 *
 * Chaque thread Lazy SMP exécute cette même fonction.
 * La seule chose partagée est la TT (lock-free).
 * Pas de killer/history partagés = pas de contention.
 */

// Killer Moves : thread-local (chaque thread a ses propres killers)
static const int MAX_PLY = 64;
struct ThreadLocal {
    Move killers[MAX_PLY][2];
    Sint32 history[64][64];
};

static inline void initThreadLocal(ThreadLocal &tl) {
    std::memset(tl.killers, -1, sizeof(tl.killers));
    std::memset(tl.history, 0, sizeof(tl.history));
}

static inline Sint32 historyScore(const ThreadLocal &tl, const Move &mv) {
    return tl.history[mv.from][mv.to];
}

static void orderRemainingMoves(const Board &b, Bitboard bb, Bitboard legals,
                                const Move &ttMove, const ThreadLocal &tl, int ply,
                                std::vector<Move> &moves) {
    std::vector<RankedMove> rankedMoves;
    rankedMoves.reserve(64);

    for (Bitboard it = bb; it; it &= (it - 1)) {
        int from = next(it);

        for (Bitboard c = validClones[from] & legals; c; c &= (c - 1)) {
            Move mv = {from, next(c)};
            if (sameMove(mv, ttMove))
                continue;
            if (ply < MAX_PLY &&
                (sameMove(mv, tl.killers[ply][0]) || sameMove(mv, tl.killers[ply][1]))) {
                continue;
            }
            rankedMoves.push_back({mv, historyScore(tl, mv)});
        }

        for (Bitboard j = validJumps[from] & legals; j; j &= (j - 1)) {
            Move mv = {from, next(j)};
            if (sameMove(mv, ttMove))
                continue;
            if (ply < MAX_PLY &&
                (sameMove(mv, tl.killers[ply][0]) || sameMove(mv, tl.killers[ply][1]))) {
                continue;
            }
            rankedMoves.push_back({mv, historyScore(tl, mv)});
        }
    }

    std::sort(rankedMoves.begin(), rankedMoves.end(),
              [](const RankedMove &a, const RankedMove &b) {
                  return a.score > b.score;
              });

    moves.clear();
    moves.reserve(rankedMoves.size());
    for (const RankedMove &rm : rankedMoves)
        moves.push_back(rm.mv);
}

static Sint32 negamax(const Board &b, Uint16 player, int depth,
                      Sint32 alpha, Sint32 beta, int ply, Uint64 key,
                      ThreadLocal &tl) {
    NODES++;

    // --- TT Lookup ---
    Sint32 alphaOrig = alpha;
    TTEntry *entry = probeTT(key);
    Move ttMove = invalidMove();

    if (entry) {
        ttMove = entry->best;
        if (entry->depth >= depth) {
            if (entry->flag == TT_EXACT)
                return entry->value;
            if (entry->flag == TT_LOWER)
                alpha = std::max(alpha, entry->value);
            else if (entry->flag == TT_UPPER)
                beta = std::min(beta, entry->value);
            if (alpha >= beta)
                return entry->value;
        }
    }

    // --- Horizon ---
    if (depth == 0) {
        Sint32 v = evaluate(b, player);
        storeTT(key, 0, v, TT_EXACT, invalidMove());
        return v;
    }

    Uint16 toMove = b.turn;
    Bitboard bb = (toMove == RED) ? b.red : b.blue;
    Bitboard legals = allowed(b);
    bool foundMove = false;
    Move bestMove = invalidMove();

    // Tour du joueur que l'on optimise.
    if (toMove == player) {
        Sint32 best = -INF;

        auto searchMove = [&](const Move &mv) -> bool {
            Board child = ::applyMove(b, mv);
            Uint64 ck = hashAfterMove(key, b, mv);
            Sint32 score = negamax(child, player, depth - 1, alpha, beta, ply + 1, ck, tl);
            foundMove = true;
            if (score > best) {
                best = score;
                bestMove = mv;
            }
            if (score > alpha)
                alpha = score;
            if (alpha >= beta) {
                // Killer update
                if (ply < MAX_PLY && !sameMove(mv, tl.killers[ply][0])) {
                    tl.killers[ply][1] = tl.killers[ply][0];
                    tl.killers[ply][0] = mv;
                }
                tl.history[mv.from][mv.to] += depth * depth;
                if (tl.history[mv.from][mv.to] > 50000)
                    tl.history[mv.from][mv.to] = 50000;
                return true;
            }
            return false;
        };

        // 1. TT move
        if (isLegalMove(b, toMove, ttMove) && searchMove(ttMove)) {
            storeTT(key, depth, best, TT_LOWER, bestMove);
            return best;
        }

        // 2. Killer moves
        for (int k = 0; k < 2 && ply < MAX_PLY; ++k) {
            Move km = tl.killers[ply][k];
            if (hasMove(km) && !sameMove(km, ttMove) && isLegalMove(b, toMove, km)) {
                if (searchMove(km)) {
                    storeTT(key, depth, best, TT_LOWER, bestMove);
                    return best;
                }
            }
        }

        // 3. Tous les autres coups, triés par history heuristic
        std::vector<Move> orderedMoves;
        orderRemainingMoves(b, bb, legals, ttMove, tl, ply, orderedMoves);
        for (const Move &mv : orderedMoves) {
            if (searchMove(mv)) {
                storeTT(key, depth, best, TT_LOWER, bestMove);
                return best;
            }
        }

        if (!foundMove) {
            if (!playerCanMove(b, opponent(toMove))) {
                Sint32 v = fatality(b, player);
                storeTT(key, depth, v, TT_EXACT, invalidMove());
                return v;
            }
            Board child = b;
            child.turn = opponent(toMove);
            Sint32 v = negamax(child, player, depth - 1, alpha, beta, ply + 1, key ^ zobristTurn, tl);
            storeTT(key, depth, v, TT_EXACT, invalidMove());
            return v;
        }

        int flag = TT_EXACT;
        if (best <= alphaOrig)
            flag = TT_UPPER;
        else if (best >= beta)
            flag = TT_LOWER;
        storeTT(key, depth, best, flag, bestMove);
        return best;
    }
    // Tour adverse.
    else {
        Sint32 best = INF;

        auto searchMove = [&](const Move &mv) -> bool {
            Board child = ::applyMove(b, mv);
            Uint64 ck = hashAfterMove(key, b, mv);
            Sint32 score = negamax(child, player, depth - 1, alpha, beta, ply + 1, ck, tl);
            foundMove = true;
            if (score < best) {
                best = score;
                bestMove = mv;
            }
            if (score < beta)
                beta = score;
            if (alpha >= beta) {
                if (ply < MAX_PLY && !sameMove(mv, tl.killers[ply][0])) {
                    tl.killers[ply][1] = tl.killers[ply][0];
                    tl.killers[ply][0] = mv;
                }
                tl.history[mv.from][mv.to] += depth * depth;
                if (tl.history[mv.from][mv.to] > 50000)
                    tl.history[mv.from][mv.to] = 50000;
                return true;
            }
            return false;
        };

        if (isLegalMove(b, toMove, ttMove) && searchMove(ttMove)) {
            storeTT(key, depth, best, TT_UPPER, bestMove);
            return best;
        }

        for (int k = 0; k < 2 && ply < MAX_PLY; ++k) {
            Move km = tl.killers[ply][k];
            if (hasMove(km) && !sameMove(km, ttMove) && isLegalMove(b, toMove, km)) {
                if (searchMove(km)) {
                    storeTT(key, depth, best, TT_UPPER, bestMove);
                    return best;
                }
            }
        }

        std::vector<Move> orderedMoves;
        orderRemainingMoves(b, bb, legals, ttMove, tl, ply, orderedMoves);
        for (const Move &mv : orderedMoves) {
            if (searchMove(mv)) {
                storeTT(key, depth, best, TT_UPPER, bestMove);
                return best;
            }
        }

        if (!foundMove) {
            if (!playerCanMove(b, opponent(toMove))) {
                Sint32 v = fatality(b, player);
                storeTT(key, depth, v, TT_EXACT, invalidMove());
                return v;
            }
            Board child = b;
            child.turn = opponent(toMove);
            Sint32 v = negamax(child, player, depth - 1, alpha, beta, ply + 1, key ^ zobristTurn, tl);
            storeTT(key, depth, v, TT_EXACT, invalidMove());
            return v;
        }

        int flag = TT_EXACT;
        if (best <= alphaOrig)
            flag = TT_UPPER;
        else if (best >= beta)
            flag = TT_LOWER;
        storeTT(key, depth, best, flag, bestMove);
        return best;
    }
}
// Recherche a la racine

static void generateMoves(const Board &b, Bitboard bb, std::vector<Move> &moves) {
    Bitboard legals = allowed(b);
    moves.clear();
    for (; bb; bb &= (bb - 1)) {
        int from = next(bb);
        for (Bitboard c = validClones[from] & legals; c; c &= (c - 1))
            moves.push_back({from, next(c)});
        for (Bitboard j = validJumps[from] & legals; j; j &= (j - 1))
            moves.push_back({from, next(j)});
    }
}

static void orderingRoot(std::vector<Move> &moves, const Move *ttMove,
                         const std::vector<Move> &ranking) {
    std::size_t pos = 0;
    if (ttMove && hasMove(*ttMove)) {
        for (std::size_t i = 0; i < moves.size(); ++i) {
            if (sameMove(*ttMove, moves[i])) {
                if (i != pos)
                    std::swap(moves[pos], moves[i]);
                ++pos;
                break;
            }
        }
    }
    for (std::size_t p = 0; p < ranking.size() && pos < moves.size(); ++p) {
        for (std::size_t i = pos; i < moves.size(); ++i) {
            if (sameMove(ranking[p], moves[i])) {
                if (i != pos)
                    std::swap(moves[pos], moves[i]);
                ++pos;
                break;
            }
        }
    }
}

// Recherche racine standard (utilisée par le thread principal ET les helpers)
static movement alphabeta(const Board &b, Bitboard bb, int searchDepth,
                          std::vector<Move> &ranking, ThreadLocal &tl) {
    Move best = invalidMove();
    std::vector<Move> moves;
    std::vector<RankedMove> rankedMoves;
    Sint32 alpha = -INF;
    Sint32 beta = INF;
    Uint16 player = b.turn;
    Uint64 key = hashBoard(b);

    TTEntry *entry = probeTT(key);
    Move ttMove = invalidMove();
    if (entry)
        ttMove = entry->best;

    generateMoves(b, bb, moves);
    orderingRoot(moves, hasMove(ttMove) ? &ttMove : nullptr, ranking);
    rankedMoves.reserve(moves.size());

    Sint32 bestScore = -INF;

    for (std::size_t i = 0; i < moves.size(); ++i) {
        Board child = ::applyMove(b, moves[i]);
        Uint64 ck = hashAfterMove(key, b, moves[i]);
        Sint32 score = negamax(child, player, searchDepth - 1, alpha, beta, 1, ck, tl);
        rankedMoves.push_back({moves[i], score});

        if (best.from == -1 || score > bestScore) {
            best = moves[i];
            bestScore = score;
        }
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }

    // Trier pour le ranking de l'itération suivante
    std::sort(rankedMoves.begin(), rankedMoves.end(),
              [](const RankedMove &a, const RankedMove &b) {
                  return a.score > b.score;
              });

    ranking.clear();
    for (auto &rm : rankedMoves)
        ranking.push_back(rm.mv);

    if (!hasMove(best))
        return movement(0, 0, 0, 0);

    storeTT(key, searchDepth, bestScore, TT_EXACT, best);
    return movement(X(best.from), Y(best.from), X(best.to), Y(best.to));
}
/*
 * LAZY SMP — La technique de Stockfish
 *
 * Principe : N threads exécutent chacun un alpha-beta séquentiel COMPLET
 * (avec toutes ses coupures alpha-beta intactes).
 * Ils partagent uniquement la TT (lock-free).
 *
 * Pourquoi ça marche :
 * - Chaque thread a ses propres killers/history -> pas de contention
 * - Les threads explorent des parties différentes de l'arbre naturellement
 *   (petites variations dues à l'ordre d'insertion TT)
 * - Quand thread A trouve un bon coup et le stocke dans la TT,
 *   thread B le trouve via probeTT et coupe plus vite
 * - Zéro perte de coupure alpha-beta (contrairement au YBWC à la racine)
 *
 * Pour diversifier la recherche, les helpers cherchent à depth+1
 * (ils explorent plus profond, enrichissant la TT pour le thread principal)
 */

static void initValid() {
    for (int from = 0; from < 64; ++from) {
        int fx = X(from), fy = Y(from);
        validClones[from] = 0;
        validJumps[from] = 0;
        for (int x = std::max(0, fx - 2); x <= std::min(7, fx + 2); ++x)
            for (int y = std::max(0, fy - 2); y <= std::min(7, fy + 2); ++y) {
                if (x == fx && y == fy)
                    continue;
                int d = std::max(std::abs(x - fx), std::abs(y - fy));
                int to = index(x, y);
                if (d == 1)
                    validClones[from] |= mask(to);
                else if (d == 2)
                    validJumps[from] |= mask(to);
            }
    }
}

static inline void initialize(const Board &b) {
    if (init_done)
        return;
    initValid();
    initZobrist();
    init_done = true;
}
// Logs de recherche

static const char *mapNameFromHoles(Bitboard holes) {
    switch (holes) {
    case 0x0000000000000000ULL:
        return "standard";
    case 0x0042241818244200ULL:
        return "x";
    case 0x0048012408401200ULL:
        return "irregular";
    case 0x005a245a5a245a00ULL:
        return "rings";
    case 0x0078444242221e00ULL:
        return "island";
    case 0x007e7e62627e7e00ULL:
        return "fortress";
    case 0x007e7e66247e7e00ULL:
        return "inside";
    case 0x080808f81f101010ULL:
        return "cross";
    case 0x1818001818001818ULL:
        return "quantum";
    case 0x3037342020343730ULL:
        return "strange";
    case 0x54aa55aa55aa552aULL:
        return "chess";
    case 0x5adb18e7e718db5aULL:
        return "constrained";
    case 0x7e99e72424e7997eULL:
        return "path";
    case 0x7effffffffffff7cULL:
        return "test";
    default:
        return "unknown";
    }
}
// Entree appelee par le moteur

void Strategy::computeBestMove() {
    Board b = makeBoard(_blobs, _holes);
    initialize(b);
    b.turn = _current_player;

    Bitboard bb = (b.turn == RED) ? b.red : b.blue;

    const char *mapId = mapNameFromHoles(b.holes);
    std::cerr << "[ULTRAPAR] Lazy SMP threads=" << NUM_THREADS
              << " TT=" << (TT_SIZE >> 10) << "K"
              << " player=" << playerName(b.turn)
              << " map=" << mapId << std::endl;

    // --- Thread principal : iterative deepening classique ---
    std::vector<Move> mainRanking;
    ThreadLocal mainTL;
    initThreadLocal(mainTL);

    movement prevMv(0, 0, 0, 0);
    uint64_t prevNodes = 0;
    int stagnation = 0;

    // Signal d'arrêt pour les threads helpers
    std::atomic<bool> stopHelpers{false};

    /*
     * --- Lancer les helpers Lazy SMP ---
     * Chaque helper fait son propre iterative deepening en boucle,
     * partageant uniquement la TT avec le thread principal.
     */
    std::vector<std::thread> helpers;
    int numHelpers = NUM_THREADS - 1;

    for (int h = 0; h < numHelpers; ++h) {
        helpers.emplace_back([&, h]() {
            ThreadLocal helperTL;
            initThreadLocal(helperTL);
            std::vector<Move> helperRanking;

            // Diversifier : chaque helper démarre à une profondeur légèrement différente
            int startDepth = 1 + (h % 3);

            for (int depth = startDepth; !stopHelpers.load(std::memory_order_relaxed); ++depth) {
                ::alphabeta(b, bb, depth, helperRanking, helperTL);
            }
        });
    }

    // --- Thread principal : iterative deepening ---
    for (int depth = 1;; ++depth) {
        uint64_t nodesBefore = NODES;
        auto start = std::chrono::high_resolution_clock::now();

        movement mv = ::alphabeta(b, bb, depth, mainRanking, mainTL);

        auto end = std::chrono::high_resolution_clock::now();
        uint64_t deltaN = NODES - nodesBefore;
        double elapsedMs = std::chrono::duration<double>(end - start).count() * 1000.0;
        double nps = (elapsedMs > 0) ? deltaN / (elapsedMs / 1000.0) : 0;

        if (!(mv.ox == mv.nx && mv.oy == mv.ny)) {
            std::cerr << "[ULTRAPAR]"
                      << " player=" << playerName(b.turn)
                      << " D:" << depth
                      << " | t:" << (int)elapsedMs << "ms"
                      << " | N:" << deltaN
                      << " | NPS:" << (int)(nps / 1000.0) << "k"
                      << std::endl;

            _saveBestMove(mv);

            if (mv.ox == prevMv.ox && mv.oy == prevMv.oy &&
                mv.nx == prevMv.nx && mv.ny == prevMv.ny &&
                deltaN == prevNodes) {
                stagnation++;
            } else {
                stagnation = 0;
            }
            prevMv = mv;
            prevNodes = deltaN;
            if (stagnation >= 2)
                break;
        }
    }

    // Arrêter les helpers
    stopHelpers.store(true, std::memory_order_relaxed);
    for (auto &t : helpers)
        t.join();
}
void Strategy::applyMove(const movement &) {}
Sint32 Strategy::estimateCurrentScore() const { return 0; }
vector<movement> &Strategy::computeValidMoves(vector<movement> &v) const { return v; }
