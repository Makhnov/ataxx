#ifndef MM0_STRATEGY_STRATEGY_H
#define MM0_STRATEGY_STRATEGY_H

#include "common/config.h"
#include "common/types.h"

#include <atomic>
#include <string>

namespace mm0::strategy {
/*
 * Initialise une seule fois les tables de coups et les clés Zobrist.
 * La TT n'est pas touchée ici: elle est gérée au début des recherches.
 */
void init();
/*
 * Résultat partagé entre la recherche et le moteur.
 * La recherche publie le meilleur coup connu après chaque profondeur terminée.
 * Les champs reply_* servent pour l'anticipation du coup adverse.
 * Les atomics évitent d'ajouter un verrou juste pour lire le dernier résultat.
 */
struct Result {
    std::atomic<int> best_from{-1}; // indice de case, -1 si aucun coup
    std::atomic<int> best_to{-1};
    std::atomic<int> reply_from{-1}; // réponse adverse prédite
    std::atomic<int> reply_to{-1};
    std::atomic<Sint32> best_score{0};
    std::atomic<int> depth_reached{0};
    std::atomic<uint64_t> total_nodes{0};

    void reset() {
        best_from.store(-1, std::memory_order_relaxed);
        best_to.store(-1, std::memory_order_relaxed);
        reply_from.store(-1, std::memory_order_relaxed);
        reply_to.store(-1, std::memory_order_relaxed);
        best_score.store(0, std::memory_order_relaxed);
        depth_reached.store(0, std::memory_order_relaxed);
        total_nodes.store(0, std::memory_order_relaxed);
    }
};

struct SearchContext {
    std::string algo_id = config::ALGO_ID;
    std::string algo_name = config::ALGO_NAME;
    std::string map_name = "standard";
    std::string log_dir = "../logs/tournament";
    bool log_files = true;

    std::string phase = "my_turn";
    std::string occupied_phase = "opening";
    int turn_index = 0;
    int occupied = 0;
    int playable = 64;
    int occupied_pct = 0;
    int red = 0;
    int blue = 0;
    std::string perspective = "RED";
    std::string root_turn = "RED";
};
/*
 * Point d'entrée principal de la stratégie.
 *
 * holes contient les trous du plateau, red et blue les deux bitboards courants.
 * side_to_move indique qui joue maintenant. perspective indique pour quel joueur
 * on évalue le score, ce qui sert aussi pendant les recherches lancées sur le
 * temps adverse. Le flag stop appartient au moteur: la stratégie le consulte
 * seulement pour sortir proprement quand le temps est terminé ou quand le coup
 * adverse vient d'arriver.
 *
 * La recherche s'arrête aussi si deux itérations donnent exactement le même
 * résultat. Dans ce cas, continuer ne change presque jamais le coup joué.
 * La table de transposition est remise à zéro au début de chaque appel.
 */
void compute_best_move(Bitboard holes,
                       Bitboard red,
                       Bitboard blue,
                       Player side_to_move,
                       Player perspective,
                       std::atomic<bool> &stop,
                       Result &out,
                       const SearchContext &ctx = SearchContext{});

} // namespace mm0::strategy

#endif
