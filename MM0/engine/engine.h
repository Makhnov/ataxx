#ifndef MM0_ENGINE_ENGINE_H
#define MM0_ENGINE_ENGINE_H

#include "common/config.h"
#include "common/types.h"
#include "strategy/strategy.h"
#include "time/clock.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace mm0 {

class Client;

class Engine {
public:
    enum class State {
        IDLE,              // Avant le début de la partie
        MY_TURN,           // Recherche en cours
        WAIT_CONFIRMATION, // En attente de confirmation du serveur
        OPP_TURN,          // Recherche spéculative
        GAME_OVER          // Victoire !... (ou pas ^^)
    };

    explicit Engine(const config::RuntimeOptions &options = config::RuntimeOptions{});
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    void run();
    void request_shutdown() { shutdown_.store(true, std::memory_order_relaxed); }

    // Callbacks du client
    void on_welcome(const char *map_name);
    void on_map_change(const char *map_name);
    void on_start(int local_player_id);
    void on_opp_move(Move m);
    void on_disconnect();

private:
    // Changements d'états
    void enter_my_turn();
    void enter_opp_turn();
    void publish_move_and_wait();
    void apply_opp_move_and_resume(Move m);

    // Gesgtions des threads de recherche et de l'horloge
    void launch_search(Player side_to_move, Player perspective);
    void launch_search_on(Bitboard red, Bitboard blue, Player side_to_move, Player perspective);
    void stop_and_join_search();

    // Gestion principale (envoie du coup, etc.)
    void apply_move_local(int from, int to);
    bool can_move_local(Player player) const;
    bool is_legal_move_local(Player player, Move m) const;
    void dispatch_current_turn();
    bool try_launch_shortcut_search();
    Move read_predicted_opp_reply() const;
    void invalidate_shortcut();
    void maybe_send_pregame_commands();
    void log_final_score(const char *reason);
    int count_blobs(Player player) const;

    State state_ = State::IDLE;
    config::RuntimeOptions options_;

    // Etat du plateau, du tout...
    Bitboard red_ = 0;
    Bitboard blue_ = 0;
    Bitboard holes_ = 0;
    Player turn_ = RED;
    std::string map_name_ = "standard";

    // whoami
    Player me_ = RED;
    int local_player_id_ = -1;

    // Thread de recherche et résultat partagé
    std::thread search_thread_;
    std::atomic<bool> search_stop_{true}; // true = no search running / must stop
    strategy::Result result_;             // published by search thread, read by engine

    // Recherche spéculative
    Move predicted_opp_reply_{}; // Obtenu à mon tour précédent
    Move shortcut_opp_move_{};   // Spéculation
    bool shortcut_search_active_ = false;
    int shortcut_hits_ = 0;
    int shortcut_misses_ = 0;
    int shortcut_no_prediction_ = 0;
    int shortcut_illegal_ = 0;
    int shortcut_depth_at_hit_ = -1;
    std::string pending_shortcut_tag_ = "none";

    // Uniquement durant la recherche (côté shortcut c'est le message du serveur qui arrête la spéculation)
    Clock clock_{};

    std::unique_ptr<Client> client_;

    std::atomic<bool> shutdown_{false};

    // Compteurs de tours et de coups joués, principalement pour les logs
    int turn_counter_ = 0;
    int move_counter_ = 0;
    bool map_command_sent_ = false;
    bool start_command_sent_ = false;
    std::chrono::steady_clock::time_point next_start_attempt_{};
};

} // namespace mm0

#endif
