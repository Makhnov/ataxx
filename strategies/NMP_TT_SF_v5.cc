/*
 * strategy.cc — version alpha-beta bitboard.
 *
 * Cette version garde une table de transposition fraîche à chaque recherche.
 * Elle publie aussi, quand c'est possible, un coup adverse probable pour que le
 * moteur puisse commencer à réfléchir pendant le tour de l'autre joueur.
 *
 * Le std::atomic<bool>& stop est consulté dans l'iterative deepening et dans
 * negamax. L'algo ne décide pas lui-même d'arrêter: il sort proprement quand le
 * moteur lui signale que le temps est terminé ou qu'un coup adverse est arrivé.
 */

#include "strategy/strategy.h"

#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mm0::strategy {

/*
 * Types locaux utilisés par la recherche.
 * On garde des int simples pour les cases afin de rester proche des autres
 * versions bitboard du dossier.
 */
using mm0::Bitboard;
using mm0::BLUE;
using mm0::Player;
using mm0::RED;

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

// Table de transposition, partagée entre les threads.

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
static Bitboard validClones[64];
static Bitboard validJumps[64];

// TT de 1M d'entrées, remise à zéro à chaque recherche.
static const std::size_t TT_SIZE = 1u << 20;
static TTEntry transTable[TT_SIZE];

// Hachage Zobrist.
static Uint64 zobristPiece[2][64];
static Uint64 zobristTurn;
static bool zobrist_init = false;

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
static inline bool sameMove(const Move &a, const Move &b) {
    return a.from == b.from && a.to == b.to;
}

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

// Mise à jour incrémentale du hash après un coup.
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

// Accès à la TT. On accepte les petites races de lecture/écriture.
static inline TTEntry *probeTT(Uint64 key) {
    TTEntry &e = transTable[key & (TT_SIZE - 1)];
    if (e.flag != TT_EMPTY && e.key == key)
        return &e;
    return nullptr;
}

struct SearchCounters {
    enum OrderSource {
        ORDER_TT_MOVE = 0,
        ORDER_MEMORY = 1,
        ORDER_KILLER = 2,
        ORDER_HISTORY = 3,
        ORDER_SOURCE_COUNT = 4,
    };

    uint64_t nodes = 0;
    uint64_t probes = 0;
    uint64_t hits = 0;
    uint64_t cutoffs = 0;
    uint64_t moveHits = 0;
    uint64_t legalMoves = 0;
    uint64_t exactHits = 0;
    uint64_t newEntries = 0;
    uint64_t overwrites = 0;
    uint64_t rejected = 0;
    uint64_t orderTriedMoves = 0;
    uint64_t orderCutoffs = 0;
    uint64_t orderFirstCutoffs = 0;
    uint64_t orderCutIndexSum = 0;
    uint64_t orderTried[ORDER_SOURCE_COUNT] = {};
    uint64_t orderCut[ORDER_SOURCE_COUNT] = {};
    uint64_t orderCutDepthSum[ORDER_SOURCE_COUNT] = {};
    uint64_t rootIterations = 0;
    uint64_t rootFirstBest = 0;
    uint64_t rootBestRankSum = 0;
    uint64_t rootBoundUpdates = 0;
    uint64_t rootMoveCountSum = 0;

    void add(const SearchCounters &other) {
        nodes += other.nodes;
        probes += other.probes;
        hits += other.hits;
        cutoffs += other.cutoffs;
        moveHits += other.moveHits;
        legalMoves += other.legalMoves;
        exactHits += other.exactHits;
        newEntries += other.newEntries;
        overwrites += other.overwrites;
        rejected += other.rejected;
        orderTriedMoves += other.orderTriedMoves;
        orderCutoffs += other.orderCutoffs;
        orderFirstCutoffs += other.orderFirstCutoffs;
        orderCutIndexSum += other.orderCutIndexSum;
        for (int i = 0; i < ORDER_SOURCE_COUNT; ++i) {
            orderTried[i] += other.orderTried[i];
            orderCut[i] += other.orderCut[i];
            orderCutDepthSum[i] += other.orderCutDepthSum[i];
        }
        rootIterations += other.rootIterations;
        rootFirstBest += other.rootFirstBest;
        rootBestRankSum += other.rootBestRankSum;
        rootBoundUpdates += other.rootBoundUpdates;
        rootMoveCountSum += other.rootMoveCountSum;
    }
};

static inline void recordOrderTry(SearchCounters &stats, SearchCounters::OrderSource source) {
    stats.orderTriedMoves++;
    stats.orderTried[source]++;
}

static inline void recordOrderCut(SearchCounters &stats, SearchCounters::OrderSource source,
                                  int depth, int cutIndex) {
    stats.orderCutoffs++;
    stats.orderCut[source]++;
    stats.orderCutDepthSum[source] += static_cast<uint64_t>(std::max(depth, 0));
    stats.orderCutIndexSum += static_cast<uint64_t>(std::max(cutIndex, 0));
    if (cutIndex == 0)
        stats.orderFirstCutoffs++;
}

static inline void storeTT(Uint64 key, int depth, Sint32 value, int flag, const Move &best,
                           SearchCounters *stats = nullptr) {
    TTEntry &e = transTable[key & (TT_SIZE - 1)];
    // Replace-by-depth: ne pas écraser une entrée plus profonde.
    if (e.flag != TT_EMPTY) {
        if (e.key == key && e.depth > depth) {
            if (stats)
                stats->rejected++;
            return;
        }
        if (e.key != key && e.depth > depth) {
            if (stats)
                stats->rejected++;
            return;
        }
        if (stats)
            stats->overwrites++;
    } else if (stats) {
        stats->newEntries++;
    }
    e.key = key;
    e.depth = depth;
    e.value = value;
    e.flag = flag;
    e.best = best;
}

static inline void clearTT() {
    std::fill(transTable, transTable + TT_SIZE, TTEntry{});
}

// Manipulation du plateau
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

// Évaluation de position
static inline Sint32 fatality(const Board &b, Uint16 player, int depth) {
    Sint32 me = popcount((player == RED) ? b.red : b.blue);
    Sint32 opp = popcount((player == RED) ? b.blue : b.red);
    Sint32 margin = me - opp;
    if (margin > 0)
        return 50000 + 100 * margin + depth;
    if (margin < 0)
        return -50000 + 100 * margin - depth;
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

// Recherche negamax
static const int MAX_PLY = 64;
struct ThreadLocal {
    Move killers[MAX_PLY][2];
    Sint32 history[64][64];
    SearchCounters stats;
    bool stopped;
};

static inline void initThreadLocal(ThreadLocal &tl) {
    std::memset(tl.killers, -1, sizeof(tl.killers));
    std::memset(tl.history, 0, sizeof(tl.history));
    tl.stats = SearchCounters{};
    tl.stopped = false;
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
                      ThreadLocal &tl, const std::atomic<bool> &stop) {
    if (stop.load(std::memory_order_relaxed)) {
        tl.stopped = true;
        return evaluate(b, player);
    }

#ifndef MM0_FULLSPEED_BUILD
    tl.stats.nodes++;
#endif

    // Lecture de la TT.
    Sint32 alphaOrig = alpha;
    Sint32 betaOrig = beta;
#ifndef MM0_FULLSPEED_BUILD
    tl.stats.probes++;
#endif
    TTEntry *entry = probeTT(key);
    Move ttMove = invalidMove();

    if (entry) {
#ifndef MM0_FULLSPEED_BUILD
        tl.stats.hits++;
#endif
        ttMove = entry->best;
        if (hasMove(ttMove)) {
#ifndef MM0_FULLSPEED_BUILD
            tl.stats.moveHits++;
#endif
            if (isLegalMove(b, b.turn, ttMove)) {
#ifndef MM0_FULLSPEED_BUILD
                tl.stats.legalMoves++;
#endif
            }
        }
        if (entry->depth >= depth) {
            if (entry->flag == TT_EXACT) {
#ifndef MM0_FULLSPEED_BUILD
                tl.stats.exactHits++;
#endif
                return entry->value;
            }
            if (entry->flag == TT_LOWER)
                alpha = std::max(alpha, entry->value);
            else if (entry->flag == TT_UPPER)
                beta = std::min(beta, entry->value);
            if (alpha >= beta) {
#ifndef MM0_FULLSPEED_BUILD
                tl.stats.cutoffs++;
#endif
                return entry->value;
            }
        }
    }

    // Profondeur atteinte.
    if (depth == 0) {
        Sint32 v = evaluate(b, player);
#ifndef MM0_FULLSPEED_BUILD
        storeTT(key, 0, v, TT_EXACT, invalidMove(), &tl.stats);
#else
        storeTT(key, 0, v, TT_EXACT, invalidMove());
#endif
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
#ifndef MM0_FULLSPEED_BUILD
        int triedIndex = 0;
#endif

        auto searchMove = [&](const Move &mv) -> bool {
            Board child = applyMove(b, mv);
            Uint64 ck = hashAfterMove(key, b, mv);
            Sint32 score = negamax(child, player, depth - 1, alpha, beta, ply + 1, ck, tl, stop);
            if (tl.stopped)
                return true;
            foundMove = true;
            if (score > best) {
                best = score;
                bestMove = mv;
            }
            if (score > alpha)
                alpha = score;
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

        // 1. Coup proposé par la TT.
        if (isLegalMove(b, toMove, ttMove)) {
#ifndef MM0_FULLSPEED_BUILD
            const int cutIndex = triedIndex++;
            recordOrderTry(tl.stats, SearchCounters::ORDER_TT_MOVE);
#endif
            if (searchMove(ttMove)) {
                if (tl.stopped)
                    return best;
#ifndef MM0_FULLSPEED_BUILD
                recordOrderCut(tl.stats, SearchCounters::ORDER_TT_MOVE, depth, cutIndex);
                storeTT(key, depth, best, TT_LOWER, bestMove, &tl.stats);
#else
                storeTT(key, depth, best, TT_LOWER, bestMove);
#endif
                return best;
            }
        }

        // 2. Killer moves.
        for (int k = 0; k < 2 && ply < MAX_PLY; ++k) {
            Move km = tl.killers[ply][k];
            if (hasMove(km) && !sameMove(km, ttMove) && isLegalMove(b, toMove, km)) {
#ifndef MM0_FULLSPEED_BUILD
                const int cutIndex = triedIndex++;
                recordOrderTry(tl.stats, SearchCounters::ORDER_KILLER);
#endif
                if (searchMove(km)) {
                    if (tl.stopped)
                        return best;
#ifndef MM0_FULLSPEED_BUILD
                    recordOrderCut(tl.stats, SearchCounters::ORDER_KILLER, depth, cutIndex);
                    storeTT(key, depth, best, TT_LOWER, bestMove, &tl.stats);
#else
                    storeTT(key, depth, best, TT_LOWER, bestMove);
#endif
                    return best;
                }
            }
        }

        // 3. Tous les autres coups, triés par history.
        std::vector<Move> orderedMoves;
        orderRemainingMoves(b, bb, legals, ttMove, tl, ply, orderedMoves);
        for (const Move &mv : orderedMoves) {
#ifndef MM0_FULLSPEED_BUILD
            const int cutIndex = triedIndex++;
            recordOrderTry(tl.stats, SearchCounters::ORDER_HISTORY);
#endif
            if (searchMove(mv)) {
                if (tl.stopped)
                    return best;
#ifndef MM0_FULLSPEED_BUILD
                recordOrderCut(tl.stats, SearchCounters::ORDER_HISTORY, depth, cutIndex);
                storeTT(key, depth, best, TT_LOWER, bestMove, &tl.stats);
#else
                storeTT(key, depth, best, TT_LOWER, bestMove);
#endif
                return best;
            }
        }

        if (!foundMove) {
            if (!playerCanMove(b, opponent(toMove))) {
                Sint32 v = fatality(b, player, depth);
#ifndef MM0_FULLSPEED_BUILD
                storeTT(key, depth, v, TT_EXACT, invalidMove(), &tl.stats);
#else
                storeTT(key, depth, v, TT_EXACT, invalidMove());
#endif
                return v;
            }
            Board child = b;
            child.turn = opponent(toMove);
            Sint32 v = negamax(child, player, depth - 1, alpha, beta, ply + 1,
                               key ^ zobristTurn, tl, stop);
            if (tl.stopped)
                return v;
#ifndef MM0_FULLSPEED_BUILD
            storeTT(key, depth, v, TT_EXACT, invalidMove(), &tl.stats);
#else
            storeTT(key, depth, v, TT_EXACT, invalidMove());
#endif
            return v;
        }

        int flag = TT_EXACT;
        if (best <= alphaOrig)
            flag = TT_UPPER;
        else if (best >= betaOrig)
            flag = TT_LOWER;
#ifndef MM0_FULLSPEED_BUILD
        storeTT(key, depth, best, flag, bestMove, &tl.stats);
#else
        storeTT(key, depth, best, flag, bestMove);
#endif
        return best;
    }
    // Tour adverse.
    else {
        Sint32 best = INF;
#ifndef MM0_FULLSPEED_BUILD
        int triedIndex = 0;
#endif

        auto searchMove = [&](const Move &mv) -> bool {
            Board child = applyMove(b, mv);
            Uint64 ck = hashAfterMove(key, b, mv);
            Sint32 score = negamax(child, player, depth - 1, alpha, beta, ply + 1, ck, tl, stop);
            if (tl.stopped)
                return true;
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

        if (isLegalMove(b, toMove, ttMove)) {
#ifndef MM0_FULLSPEED_BUILD
            const int cutIndex = triedIndex++;
            recordOrderTry(tl.stats, SearchCounters::ORDER_TT_MOVE);
#endif
            if (searchMove(ttMove)) {
                if (tl.stopped)
                    return best;
#ifndef MM0_FULLSPEED_BUILD
                recordOrderCut(tl.stats, SearchCounters::ORDER_TT_MOVE, depth, cutIndex);
                storeTT(key, depth, best, TT_UPPER, bestMove, &tl.stats);
#else
                storeTT(key, depth, best, TT_UPPER, bestMove);
#endif
                return best;
            }
        }

        for (int k = 0; k < 2 && ply < MAX_PLY; ++k) {
            Move km = tl.killers[ply][k];
            if (hasMove(km) && !sameMove(km, ttMove) && isLegalMove(b, toMove, km)) {
#ifndef MM0_FULLSPEED_BUILD
                const int cutIndex = triedIndex++;
                recordOrderTry(tl.stats, SearchCounters::ORDER_KILLER);
#endif
                if (searchMove(km)) {
                    if (tl.stopped)
                        return best;
#ifndef MM0_FULLSPEED_BUILD
                    recordOrderCut(tl.stats, SearchCounters::ORDER_KILLER, depth, cutIndex);
                    storeTT(key, depth, best, TT_UPPER, bestMove, &tl.stats);
#else
                    storeTT(key, depth, best, TT_UPPER, bestMove);
#endif
                    return best;
                }
            }
        }

        std::vector<Move> orderedMoves;
        orderRemainingMoves(b, bb, legals, ttMove, tl, ply, orderedMoves);
        for (const Move &mv : orderedMoves) {
#ifndef MM0_FULLSPEED_BUILD
            const int cutIndex = triedIndex++;
            recordOrderTry(tl.stats, SearchCounters::ORDER_HISTORY);
#endif
            if (searchMove(mv)) {
                if (tl.stopped)
                    return best;
#ifndef MM0_FULLSPEED_BUILD
                recordOrderCut(tl.stats, SearchCounters::ORDER_HISTORY, depth, cutIndex);
                storeTT(key, depth, best, TT_UPPER, bestMove, &tl.stats);
#else
                storeTT(key, depth, best, TT_UPPER, bestMove);
#endif
                return best;
            }
        }

        if (!foundMove) {
            if (!playerCanMove(b, opponent(toMove))) {
                Sint32 v = fatality(b, player, depth);
#ifndef MM0_FULLSPEED_BUILD
                storeTT(key, depth, v, TT_EXACT, invalidMove(), &tl.stats);
#else
                storeTT(key, depth, v, TT_EXACT, invalidMove());
#endif
                return v;
            }
            Board child = b;
            child.turn = opponent(toMove);
            Sint32 v = negamax(child, player, depth - 1, alpha, beta, ply + 1,
                               key ^ zobristTurn, tl, stop);
            if (tl.stopped)
                return v;
#ifndef MM0_FULLSPEED_BUILD
            storeTT(key, depth, v, TT_EXACT, invalidMove(), &tl.stats);
#else
            storeTT(key, depth, v, TT_EXACT, invalidMove());
#endif
            return v;
        }

        int flag = TT_EXACT;
        if (best <= alphaOrig)
            flag = TT_UPPER;
        else if (best >= betaOrig)
            flag = TT_LOWER;
#ifndef MM0_FULLSPEED_BUILD
        storeTT(key, depth, best, flag, bestMove, &tl.stats);
#else
        storeTT(key, depth, best, flag, bestMove);
#endif
        return best;
    }
}

// Recherche à la racine
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

// Initialisation des tables de coups
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

void init() {
    if (init_done)
        return;
    initValid();
    initZobrist();
    init_done = true;
}

#ifndef MM0_FULLSPEED_BUILD
static double pct(uint64_t num, uint64_t den) {
    return den ? (100.0 * static_cast<double>(num) / static_cast<double>(den)) : 0.0;
}

static void appendSearchCsv(const SearchContext &ctx, const std::string &line) {
    if (!ctx.log_files)
        return;

    try {
        const std::filesystem::path raw_dir = std::filesystem::path(ctx.log_dir) / "raw";
        std::filesystem::create_directories(raw_dir);
        const std::filesystem::path path = raw_dir / (ctx.algo_id + ".search.csv");
        const bool write_header =
            !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;

        std::ofstream out(path, std::ios::app);
        if (!out)
            return;

        if (write_header) {
            out << "AlgoId;AlgoName;Map;Phase;OccupiedPhase;TurnIndex;Occupied;"
                   "Playable;OccupiedPct;Red;Blue;Perspective;RootTurn;"
                   "Depth;ElapsedMs;Nodes;NPS;BestFrom;BestTo;Score;"
                   "TTProbes;TTHits;TTHitRate;TTCutoffs;TTCutoffRate;"
                   "TTMoveHits;TTLegalMoves;TTOrderRate;TTExactHits;"
                   "TTNewEntries;TTOverwrites;TTRejected;TTFillRate;"
                   "OrderTriedTTMove;OrderCutTTMove;OrderCutDepthTTMove;"
                   "OrderTriedMemory;OrderCutMemory;OrderCutDepthMemory;"
                   "OrderTriedKiller;OrderCutKiller;OrderCutDepthKiller;"
                   "OrderTriedHistory;OrderCutHistory;OrderCutDepthHistory;"
                   "OrderTriedMoves;OrderCutoffs;OrderFirstCutoffs;OrderCutIndexSum;"
                   "RootIterations;RootFirstBest;RootBestRankSum;RootBoundUpdates;"
                   "RootMoveCountSum\n";
        }
        out << line << '\n';
    } catch (const std::exception &e) {
        LOG_INFO("appendSearchCsv failed: " << e.what());
    }
}
#endif

#if defined(MM0_DEEP_BUILD)
static std::string hexBitboard(Bitboard value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0')
        << static_cast<Uint64>(value);
    return out.str();
}

static void appendRootRankingCsv(const SearchContext &ctx, const std::string &line) {
    if (!ctx.log_files)
        return;

    try {
        const std::filesystem::path raw_dir = std::filesystem::path(ctx.log_dir) / "raw";
        std::filesystem::create_directories(raw_dir);
        const std::filesystem::path path = raw_dir / (ctx.algo_id + ".root_ranking.csv");
        const bool write_header =
            !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;

        std::ofstream out(path, std::ios::app);
        if (!out)
            return;

        if (write_header) {
            out << "AlgoId;AlgoName;Map;Phase;OccupiedPhase;TurnIndex;Occupied;"
                   "Playable;OccupiedPct;Red;Blue;Perspective;RootTurn;"
                   "RedBits;BlueBits;Holes;Depth;ElapsedMs;Nodes;NPS;"
                   "Rank;From;To;Score;RootMoveCount\n";
        }
        out << line << '\n';
    } catch (const std::exception &e) {
        LOG_INFO("appendRootRankingCsv failed: " << e.what());
    }
}

static void emitRootRanking(const SearchContext &ctx,
                            const Board &root,
                            int depth,
                            double elapsedMs,
                            uint64_t nodes,
                            double nps,
                            const std::vector<RankedMove> &rankedMoves,
                            int moveCount) {
    const int limit = std::min<int>(3, static_cast<int>(rankedMoves.size()));
    for (int i = 0; i < limit; ++i) {
        const Move &mv = rankedMoves[i].mv;
        std::ostringstream line;
        line << ctx.algo_id << ';'
             << ctx.algo_name << ';'
             << ctx.map_name << ';'
             << ctx.phase << ';'
             << ctx.occupied_phase << ';'
             << ctx.turn_index << ';'
             << ctx.occupied << ';'
             << ctx.playable << ';'
             << ctx.occupied_pct << ';'
             << ctx.red << ';'
             << ctx.blue << ';'
             << ctx.perspective << ';'
             << ctx.root_turn << ';'
             << hexBitboard(root.red) << ';'
             << hexBitboard(root.blue) << ';'
             << hexBitboard(root.holes) << ';'
             << depth << ';'
             << static_cast<int>(elapsedMs) << ';'
             << nodes << ';'
             << static_cast<uint64_t>(nps) << ';'
             << (i + 1) << ';'
             << mv.from << ';'
             << mv.to << ';'
             << rankedMoves[i].score << ';'
             << moveCount;
        appendRootRankingCsv(ctx, line.str());
    }
}
#endif

#ifndef MM0_FULLSPEED_BUILD
static void emitSearchStat(const SearchContext &ctx,
                           int depth,
                           double elapsedMs,
                           uint64_t nodes,
                           double nps,
                           const Move &bestMv,
                           Sint32 bestScore,
                           const SearchCounters &stats) {
    const double hitRate = pct(stats.hits, stats.probes);
    const double cutoffRate = pct(stats.cutoffs, stats.probes);
    const double orderRate = pct(stats.legalMoves, stats.moveHits);
    const double fillRate = pct(stats.newEntries, TT_SIZE);

    LOG_INFO("[MM0SEARCH]"
             << " algo=" << ctx.algo_id
             << " map=" << ctx.map_name
             << " phase=" << ctx.phase
             << " occupiedPhase=" << ctx.occupied_phase
             << " turn=" << ctx.turn_index
             << " occupied=" << ctx.occupied
             << " playable=" << ctx.playable
             << " occupiedPct=" << ctx.occupied_pct
             << " red=" << ctx.red
             << " blue=" << ctx.blue
             << " perspective=" << ctx.perspective
             << " rootTurn=" << ctx.root_turn
             << " depth=" << depth
             << " elapsedMs=" << static_cast<int>(elapsedMs)
             << " nodes=" << nodes
             << " nps=" << static_cast<uint64_t>(nps)
             << " bestFrom=" << bestMv.from
             << " bestTo=" << bestMv.to
             << " score=" << bestScore
             << " ttProbes=" << stats.probes
             << " ttHits=" << stats.hits
             << " ttHitRate=" << hitRate
             << " ttCutoffs=" << stats.cutoffs
             << " ttCutoffRate=" << cutoffRate
             << " ttMoveHits=" << stats.moveHits
             << " ttLegalMoves=" << stats.legalMoves
             << " ttOrderRate=" << orderRate
             << " ttExactHits=" << stats.exactHits
             << " ttNewEntries=" << stats.newEntries
             << " ttOverwrites=" << stats.overwrites
             << " ttRejected=" << stats.rejected
             << " ttFillRate=" << fillRate
             << " orderCutoffs=" << stats.orderCutoffs
             << " orderFirstCutoffs=" << stats.orderFirstCutoffs
             << " orderCutIndexSum=" << stats.orderCutIndexSum
             << " rootFirstBest=" << stats.rootFirstBest
             << " rootBoundUpdates=" << stats.rootBoundUpdates);

    std::ostringstream line;
    line << ctx.algo_id << ';'
         << ctx.algo_name << ';'
         << ctx.map_name << ';'
         << ctx.phase << ';'
         << ctx.occupied_phase << ';'
         << ctx.turn_index << ';'
         << ctx.occupied << ';'
         << ctx.playable << ';'
         << ctx.occupied_pct << ';'
         << ctx.red << ';'
         << ctx.blue << ';'
         << ctx.perspective << ';'
         << ctx.root_turn << ';'
         << depth << ';'
         << static_cast<int>(elapsedMs) << ';'
         << nodes << ';'
         << static_cast<uint64_t>(nps) << ';'
         << bestMv.from << ';'
         << bestMv.to << ';'
         << bestScore << ';'
         << stats.probes << ';'
         << stats.hits << ';'
         << std::fixed << std::setprecision(3) << hitRate << ';'
         << stats.cutoffs << ';'
         << cutoffRate << ';'
         << stats.moveHits << ';'
         << stats.legalMoves << ';'
         << orderRate << ';'
         << stats.exactHits << ';'
         << stats.newEntries << ';'
         << stats.overwrites << ';'
         << stats.rejected << ';'
         << fillRate << ';'
         << stats.orderTried[SearchCounters::ORDER_TT_MOVE] << ';'
         << stats.orderCut[SearchCounters::ORDER_TT_MOVE] << ';'
         << stats.orderCutDepthSum[SearchCounters::ORDER_TT_MOVE] << ';'
         << stats.orderTried[SearchCounters::ORDER_MEMORY] << ';'
         << stats.orderCut[SearchCounters::ORDER_MEMORY] << ';'
         << stats.orderCutDepthSum[SearchCounters::ORDER_MEMORY] << ';'
         << stats.orderTried[SearchCounters::ORDER_KILLER] << ';'
         << stats.orderCut[SearchCounters::ORDER_KILLER] << ';'
         << stats.orderCutDepthSum[SearchCounters::ORDER_KILLER] << ';'
         << stats.orderTried[SearchCounters::ORDER_HISTORY] << ';'
         << stats.orderCut[SearchCounters::ORDER_HISTORY] << ';'
         << stats.orderCutDepthSum[SearchCounters::ORDER_HISTORY] << ';'
         << stats.orderTriedMoves << ';'
         << stats.orderCutoffs << ';'
         << stats.orderFirstCutoffs << ';'
         << stats.orderCutIndexSum << ';'
         << stats.rootIterations << ';'
         << stats.rootFirstBest << ';'
         << stats.rootBestRankSum << ';'
         << stats.rootBoundUpdates << ';'
         << stats.rootMoveCountSum;
    appendSearchCsv(ctx, line.str());
}
#endif

/*
 * Point d'entrée appelé par le moteur.
 *
 * side_to_move indique qui doit jouer dans la position courante. perspective
 * indique pour quel joueur le score est évalué, ce qui sert aussi pendant les
 * recherches lancées sur le temps adverse. La TT reste remise à zéro à chaque
 * appel.
 */
void compute_best_move(Bitboard holes,
                       Bitboard red,
                       Bitboard blue,
                       Player side_to_move,
                       Player perspective,
                       std::atomic<bool> &stop,
                       Result &out,
                       const SearchContext &ctx) {
    // Reconstruire le Board utilisé par la recherche.
    Board b;
    b.red = red;
    b.blue = blue;
    b.holes = holes;
    b.turn = static_cast<Uint16>(side_to_move);

    // Garde-fou si init() n'a pas encore été appelé par le moteur.
    init();
    clearTT();

    Bitboard bb = (b.turn == RED) ? b.red : b.blue;
    Uint16 rootPlayer = b.turn;
    Uint16 me = static_cast<Uint16>(perspective);
    const bool rootMaximizes = (rootPlayer == me);
    Uint64 rootKey = hashBoard(b);

    // Générer les coups racine.
    std::vector<Move> rootMoves;
    generateMoves(b, bb, rootMoves);
    if (rootMoves.empty()) {
        // Pas de coup légal: l'engine gérera le pass.
        out.best_from.store(-1, std::memory_order_relaxed);
        out.best_to.store(-1, std::memory_order_relaxed);
        out.reply_from.store(-1, std::memory_order_relaxed);
        out.reply_to.store(-1, std::memory_order_relaxed);
        return;
    }

    // Publier un coup de sécurité (le premier) immédiatement.
    out.best_from.store(rootMoves[0].from, std::memory_order_relaxed);
    out.best_to.store(rootMoves[0].to, std::memory_order_relaxed);
    out.reply_from.store(-1, std::memory_order_relaxed);
    out.reply_to.store(-1, std::memory_order_relaxed);
    out.best_score.store(0, std::memory_order_relaxed);
    out.depth_reached.store(0, std::memory_order_relaxed);

    // Ranking pour ordering entre itérations.
    std::vector<Move> ranking;

    const int moveCount = static_cast<int>(rootMoves.size());
#ifndef MM0_FULLSPEED_BUILD
    Move prevBest = rootMoves[0];
    uint64_t prevNodes = 0;
    int stagnation = 0;

    uint64_t cumulativeNodes = 0;
    SearchCounters cumulativeStats;
#endif

    for (int depth = 1;; ++depth) {
        /*
         * Si le moteur demande l'arrêt, on sort. Le dernier best_move publié
         * reste valide.
         */
        if (stop.load(std::memory_order_relaxed))
            break;

        // Ordonner les coups racine (TT + ranking précédent).
        TTEntry *rootEntry = probeTT(rootKey);
        Move ttMove = invalidMove();
#ifndef MM0_FULLSPEED_BUILD
        SearchCounters iterationStats;
        iterationStats.probes++;
#endif
        if (rootEntry) {
#ifndef MM0_FULLSPEED_BUILD
            iterationStats.hits++;
#endif
            ttMove = rootEntry->best;
            if (hasMove(ttMove)) {
#ifndef MM0_FULLSPEED_BUILD
                iterationStats.moveHits++;
#endif
                if (isLegalMove(b, b.turn, ttMove)) {
#ifndef MM0_FULLSPEED_BUILD
                    iterationStats.legalMoves++;
#endif
                }
            }
        }
        orderingRoot(rootMoves, hasMove(ttMove) ? &ttMove : nullptr, ranking);

        std::vector<Sint32> scores(moveCount, rootMaximizes ? -INF : INF);
#ifndef MM0_FULLSPEED_BUILD
        uint64_t totalNodes = 0;

        auto startT = std::chrono::high_resolution_clock::now();
#endif

        // YBWC léger à la racine: coup #0 séquentiel pour fixer alpha.
        ThreadLocal rootTL;
        initThreadLocal(rootTL);
        {
            Board child = applyMove(b, rootMoves[0]);
            Uint64 ck = hashAfterMove(rootKey, b, rootMoves[0]);
            scores[0] = negamax(child, me, depth - 1, -INF, INF, 1, ck, rootTL, stop);
#ifndef MM0_FULLSPEED_BUILD
            totalNodes += rootTL.stats.nodes;
            iterationStats.add(rootTL.stats);
#endif
        }
        if (rootTL.stopped || stop.load(std::memory_order_relaxed))
            break;

        std::atomic<Sint32> sharedBound(scores[0]);

        // Paralléliser l'évaluation des autres coups racine.
#pragma omp parallel
        {
            ThreadLocal tl;
            initThreadLocal(tl);

#pragma omp for schedule(dynamic)
            for (int i = 1; i < moveCount; ++i) {
                if (stop.load(std::memory_order_relaxed)) {
                    tl.stopped = true;
                    continue;
                }

                Sint32 localBound = sharedBound.load(std::memory_order_relaxed);
                Board child = applyMove(b, rootMoves[i]);
                Uint64 ck = hashAfterMove(rootKey, b, rootMoves[i]);
                Sint32 score = negamax(child, me, depth - 1,
                                       rootMaximizes ? localBound : -INF,
                                       rootMaximizes ? INF : localBound,
                                       1, ck, tl, stop);
                if (tl.stopped)
                    continue;
                scores[i] = score;

                Sint32 prev = sharedBound.load(std::memory_order_relaxed);
                while (rootMaximizes ? (score > prev) : (score < prev)) {
                    if (sharedBound.compare_exchange_weak(prev, score,
                                                          std::memory_order_relaxed)) {
#ifndef MM0_FULLSPEED_BUILD
                        tl.stats.rootBoundUpdates++;
#endif
                        break;
                    }
                }
            }

#ifndef MM0_FULLSPEED_BUILD
#pragma omp critical(mm0_stats_aggregate)
            {
                totalNodes += tl.stats.nodes;
                iterationStats.add(tl.stats);
            }
#endif
        }

        /*
         * Si l'arrêt arrive pendant la zone parallèle, on ne publie pas les
         * scores de cette itération car ils peuvent être incomplets.
         */
        if (stop.load(std::memory_order_relaxed))
            break;

#ifndef MM0_FULLSPEED_BUILD
        auto endT = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double>(endT - startT).count() * 1000.0;
        double nps = (elapsedMs > 0) ? totalNodes / (elapsedMs / 1000.0) : 0;
#endif

        // Trouver le meilleur coup.
        int bestIdx = 0;
        Sint32 bestScore = scores[0];
        for (int i = 1; i < moveCount; ++i) {
            if (rootMaximizes ? (scores[i] > bestScore) : (scores[i] < bestScore)) {
                bestScore = scores[i];
                bestIdx = i;
            }
        }
#ifndef MM0_FULLSPEED_BUILD
        iterationStats.rootIterations++;
        iterationStats.rootMoveCountSum += static_cast<uint64_t>(moveCount);
        iterationStats.rootBestRankSum += static_cast<uint64_t>(bestIdx);
        if (bestIdx == 0)
            iterationStats.rootFirstBest++;
#endif

        // Mettre à jour le ranking pour l'itération suivante.
        std::vector<RankedMove> rankedMoves;
        rankedMoves.reserve(moveCount);
        for (int i = 0; i < moveCount; ++i)
            rankedMoves.push_back({rootMoves[i], scores[i]});
        std::sort(rankedMoves.begin(), rankedMoves.end(),
                  [rootMaximizes](const RankedMove &a, const RankedMove &b) {
                      return rootMaximizes ? (a.score > b.score) : (a.score < b.score);
                  });
        ranking.clear();
        for (auto &rm : rankedMoves)
            ranking.push_back(rm.mv);
#if defined(MM0_DEEP_BUILD) && !defined(MM0_FULLSPEED_BUILD)
        emitRootRanking(ctx, b, depth, elapsedMs, totalNodes, nps, rankedMoves, moveCount);
#endif

        // Publier atomiquement le best move de cette itération.
        Move bestMv = rootMoves[bestIdx];
        Move replyMv = invalidMove();
        if (rootPlayer == me && depth > 1) {
            Board replyBoard = applyMove(b, bestMv);
            Uint64 replyKey = hashAfterMove(rootKey, b, bestMv);
            if (TTEntry *replyEntry = probeTT(replyKey)) {
                Move candidate = replyEntry->best;
                if (isLegalMove(replyBoard, replyBoard.turn, candidate))
                    replyMv = candidate;
            }
        }

        out.best_from.store(bestMv.from, std::memory_order_relaxed);
        out.best_to.store(bestMv.to, std::memory_order_relaxed);
        out.reply_from.store(replyMv.from, std::memory_order_relaxed);
        out.reply_to.store(replyMv.to, std::memory_order_relaxed);
        out.best_score.store(bestScore, std::memory_order_relaxed);
        out.depth_reached.store(depth, std::memory_order_relaxed);
#ifndef MM0_FULLSPEED_BUILD
        storeTT(rootKey, depth, bestScore, TT_EXACT, bestMv, &iterationStats);
#else
        storeTT(rootKey, depth, bestScore, TT_EXACT, bestMv);
#endif
#ifndef MM0_FULLSPEED_BUILD
        cumulativeNodes += totalNodes;
        cumulativeStats.add(iterationStats);
        out.total_nodes.store(cumulativeNodes, std::memory_order_relaxed);

        std::cerr << "[SUPRAPAR]"
                  << " P:" << playerName(me)
                  << " T:" << playerName(rootPlayer)
                  << " D:" << depth
                  << " | t:" << (int)elapsedMs << "ms"
                  << " | N:" << totalNodes
                  << " | NPS:" << (int)(nps / 1000.0) << "k"
                  << " | best=(" << X(bestMv.from) << "," << Y(bestMv.from)
                  << ")->(" << X(bestMv.to) << "," << Y(bestMv.to) << ")"
                  << " | score=" << bestScore
                  << std::endl;

        emitSearchStat(ctx, depth, elapsedMs, totalNodes, nps, bestMv, bestScore,
                       cumulativeStats);
#endif

#ifndef MM0_FULLSPEED_BUILD
        // Stagnation check.
        if (sameMove(bestMv, prevBest) && totalNodes == prevNodes) {
            stagnation++;
        } else {
            stagnation = 0;
        }
        prevBest = bestMv;
        prevNodes = totalNodes;
        if (stagnation >= 2)
            break;
#endif
    }
}

} // namespace mm0::strategy
