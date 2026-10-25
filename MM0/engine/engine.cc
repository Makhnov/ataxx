#include "engine/engine.h"

#include "common/config.h"
#include "common/log.h"
#include "engine/client.h"
#include "rules/board.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

using mm0::Bitboard;
static Bitboard eng_validClones[64];
static Bitboard eng_validJumps[64];
static bool eng_valid_init = false;

inline int idx8(int x, int y) { return y * 8 + x; }
inline Bitboard bmask(int s) { return (Bitboard(1) << s); }
inline int XX(int i) { return i & 7; }
inline int YY(int i) { return i >> 3; }
inline int next(Bitboard bb) { return __builtin_ctzll((unsigned long long)bb); }
inline int count_bits(Bitboard bb) { return __builtin_popcountll((unsigned long long)bb); }
inline const char *player_name(mm0::Player p) { return (p == mm0::RED) ? "RED" : "BLUE"; }

int occupied_pct(Bitboard red, Bitboard blue, Bitboard holes) {
    const int occupied = count_bits(red) + count_bits(blue);
    const int playable = 64 - count_bits(holes);
    if (playable <= 0)
        return 100;
    return (occupied * 100) / playable;
}

const char *occupied_phase(Bitboard red, Bitboard blue, Bitboard holes) {
    const int pct = occupied_pct(red, blue, holes);
    if (pct < mm0::config::PHASE_OPENING_MAX_PCT)
        return "opening";
    if (pct < mm0::config::PHASE_MIDGAME_MAX_PCT)
        return "midgame";
    if (pct < mm0::config::PHASE_ENDGAME_MAX_PCT)
        return "endgame";
    return "terminal";
}

#ifndef MM0_FULLSPEED_BUILD
void append_csv_line(const mm0::config::RuntimeOptions &options,
                     const std::string &suffix,
                     const std::string &header,
                     const std::string &line) {
    if (!options.log_files)
        return;

    try {
        const std::filesystem::path raw_dir =
            std::filesystem::path(options.log_dir) / "raw";
        std::filesystem::create_directories(raw_dir);
        const std::filesystem::path path =
            raw_dir / (options.algo_id + "." + suffix + ".csv");
        const bool write_header =
            !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
        std::ofstream out(path, std::ios::app);
        if (!out)
            return;
        if (write_header)
            out << header << '\n';
        out << line << '\n';
    } catch (const std::exception &e) {
        LOG_INFO("append_csv_line failed: " << e.what());
    }
}
#endif

bool can_move_on(Bitboard red, Bitboard blue, Bitboard holes, mm0::Player player) {
    Bitboard bb = (player == mm0::RED) ? red : blue;
    Bitboard legals = ~(red | blue | holes);

    while (bb) {
        int from = next(bb);
        bb &= (bb - 1);
        if ((eng_validClones[from] | eng_validJumps[from]) & legals)
            return true;
    }
    return false;
}

bool is_legal_on(Bitboard red, Bitboard blue, Bitboard holes,
                 mm0::Player player, mm0::Move m) {
    if (!m.is_valid())
        return false;

    int from = int(m.from);
    int to = int(m.to);
    if (from < 0 || from >= 64 || to < 0 || to >= 64)
        return false;

    Bitboard me = (player == mm0::RED) ? red : blue;
    Bitboard toMask = bmask(to);
    if ((me & bmask(from)) == 0)
        return false;
    if ((red | blue | holes) & toMask)
        return false;
    return ((eng_validClones[from] | eng_validJumps[from]) & toMask) != 0;
}

void apply_move_on(Bitboard &red, Bitboard &blue, mm0::Player &turn, int from, int to) {
    Bitboard &me = (turn == mm0::RED) ? red : blue;
    Bitboard &opp = (turn == mm0::RED) ? blue : red;
    Bitboard toMask = bmask(to);

    if (eng_validJumps[from] & toMask)
        me &= ~bmask(from);
    me |= toMask;
    Bitboard flips = eng_validClones[to] & opp;
    opp &= ~flips;
    me |= flips;
    turn = mm0::opponent(turn);
}

void init_valid_tables_once() {
    if (eng_valid_init)
        return;
    for (int from = 0; from < 64; ++from) {
        int fx = XX(from), fy = YY(from);
        eng_validClones[from] = 0;
        eng_validJumps[from] = 0;
        for (int x = std::max(0, fx - 2); x <= std::min(7, fx + 2); ++x)
            for (int y = std::max(0, fy - 2); y <= std::min(7, fy + 2); ++y) {
                if (x == fx && y == fy)
                    continue;
                int d = std::max(std::abs(x - fx), std::abs(y - fy));
                int to = idx8(x, y);
                if (d == 1)
                    eng_validClones[from] |= bmask(to);
                else if (d == 2)
                    eng_validJumps[from] |= bmask(to);
            }
    }
    eng_valid_init = true;
}

} // namespace

namespace mm0 {

Engine::Engine(const config::RuntimeOptions &options)
    : options_(options),
      map_name_(options.map_set ? options.map : "standard"),
      client_(std::make_unique<Client>(this, options_)) {
    init_valid_tables_once();
    strategy::init();
    LOG_INFO_NONFULLSPEED("Engine ready");
}

Engine::~Engine() {
    // Il faut vérifier que la recherche est arrêtée avant
    stop_and_join_search();
}

void Engine::run() {
    LOG_INFO_NONFULLSPEED("Engine running");

    if (!client_->connect()) {
        LOG_INFO_NONFULLSPEED("Engine::run: connection failed, exiting");
        return;
    }

    while (!shutdown_.load(std::memory_order_relaxed)) {
        client_->poll();
        maybe_send_pregame_commands();

        // Check clock only in MY_TURN: when it expires, the search is done.
        if (state_ == State::MY_TURN && clock_.stopped()) {
            publish_move_and_wait();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    LOG_INFO_NONFULLSPEED("Engine shutting down");
    clock_.cancel();
    stop_and_join_search();
    if (client_->is_connected())
        client_->disconnect();
}

// Callbacks
void Engine::on_welcome(const char *map_name) {
    LOG_INFO_NONFULLSPEED("on_welcome: map=" << (map_name ? map_name : "(null)"));
    if (map_name && *map_name) {
        map_name_ = map_name;
        load_map(map_name, holes_);
    }
}

void Engine::on_map_change(const char *map_name) {
    LOG_INFO_NONFULLSPEED("on_map_change: map=" << (map_name ? map_name : "(null)"));
    if (state_ != State::IDLE) {
        LOG_INFO_NONFULLSPEED("on_map_change: ignored (game already started)");
        return;
    }
    if (map_name && *map_name) {
        map_name_ = map_name;
        load_map(map_name, holes_);
    }
}

void Engine::on_start(int local_player_id) {
    LOG_INFO_NONFULLSPEED("on_start: local_player_id=" << local_player_id);
    local_player_id_ = local_player_id;
    me_ = (local_player_id == 0) ? RED : BLUE;

    // conventions
    red_ = bmask(idx8(0, 0)) | bmask(idx8(7, 0));
    blue_ = bmask(idx8(0, 7)) | bmask(idx8(7, 7));
    turn_ = BLUE;
    turn_counter_ = 0;
    move_counter_ = 0;
    predicted_opp_reply_ = Move();
    shortcut_opp_move_ = Move();
    shortcut_search_active_ = false;
    shortcut_hits_ = 0;
    shortcut_misses_ = 0;
    shortcut_no_prediction_ = 0;
    shortcut_illegal_ = 0;
    shortcut_depth_at_hit_ = -1;
    pending_shortcut_tag_ = "none";

    LOG_INFO_NONFULLSPEED("============================================");
    LOG_INFO_NONFULLSPEED("Game start: player=" << (me_ == RED ? "RED" : "BLUE") << " (id=" << local_player_id << ")");
    LOG_INFO_NONFULLSPEED("============================================");

    dispatch_current_turn();
}

void Engine::on_opp_move(Move m) {
    LOG_INFO_NONFULLSPEED("on_opp_move: from=" << int(m.from) << " to=" << int(m.to) << " state=" << int(state_));

    if (state_ == State::WAIT_CONFIRMATION) {
        apply_move_local(m.from, m.to);
        dispatch_current_turn();
        return;
    }

    if (state_ == State::OPP_TURN) {
        apply_opp_move_and_resume(m);
        return;
    }

    LOG_INFO_NONFULLSPEED("on_opp_move: unexpected state " << int(state_) << ", ignoring");
}

void Engine::on_disconnect() {
    LOG_INFO_NONFULLSPEED("on_disconnect");
    if (state_ != State::IDLE && state_ != State::GAME_OVER) {
        log_final_score("disconnect");
    }
    shutdown_.store(true, std::memory_order_relaxed);
}

void Engine::enter_my_turn() {
    if (turn_ != me_ || !can_move_local(me_)) {
        dispatch_current_turn();
        return;
    }

    ++turn_counter_;
    LOG_INFO_NONFULLSPEED("--- MY_TURN " << turn_counter_ << " ---");
    state_ = State::MY_TURN;

    // On lance l'horloge
    clock_.arm(config::THINK_TIME_MS);

    // Et la recherche
    launch_search(turn_, me_);
}

void Engine::enter_opp_turn() {
    if (turn_ == me_ || !can_move_local(turn_)) {
        dispatch_current_turn();
        return;
    }

    LOG_INFO_NONFULLSPEED("--- OPP_TURN ---");
    state_ = State::OPP_TURN;

    // Shortcut
    if (try_launch_shortcut_search())
        return;

    if (config::SHORTCUT_FALLBACK_OPP_SEARCH) {
        LOG_INFO_NONFULLSPEED("SHORTCUT fallback: searching real opponent turn");
        launch_search(turn_, me_);
    } else {
        LOG_INFO_NONFULLSPEED("SHORTCUT fallback disabled: waiting for opponent move");
        stop_and_join_search();
        result_.reset();
        search_stop_.store(true, std::memory_order_relaxed);
    }
}

void Engine::publish_move_and_wait() {

    // Fin du temps
    stop_and_join_search();

    int from = result_.best_from.load(std::memory_order_relaxed);
    int to = result_.best_to.load(std::memory_order_relaxed);

    if (from < 0 || to < 0) {
        LOG_INFO_NONFULLSPEED("publish_move_and_wait: no move available, local skip");
        invalidate_shortcut();
        turn_ = opponent(turn_);
        dispatch_current_turn();
        return;
    }

    predicted_opp_reply_ = read_predicted_opp_reply();

    Move m;
    m.from = static_cast<Sint8>(from);
    m.to = static_cast<Sint8>(to);
    std::string shortcut_tag = pending_shortcut_tag_;

    const int depth = result_.depth_reached.load(std::memory_order_relaxed);

    if (shortcut_tag == "hit" &&
        shortcut_depth_at_hit_ >= 0 &&
        depth > shortcut_depth_at_hit_) {
        shortcut_tag = "useful_hit";
    }

// Désactivés pour la version tournoi
#ifdef MM0_FULLSPEED_BUILD
    LOG_INFO("[MM0F] turn=" << turn_counter_
                            << " move=" << (move_counter_ + 1)
                            << " depth=" << depth
                            << " shortcut=" << shortcut_tag);
#else
    LOG_INFO("MY_TURN result: (" << XX(from) << "," << YY(from)
                                 << ") -> (" << XX(to) << "," << YY(to)
                                 << ") score=" << result_.best_score.load()
                                 << " depth=" << result_.depth_reached.load()
                                 << " nodes=" << result_.total_nodes.load());
    if (predicted_opp_reply_.is_valid()) {
        LOG_INFO("SHORTCUT prediction: OPP likely ("
                 << XX(predicted_opp_reply_.from) << "," << YY(predicted_opp_reply_.from)
                 << ") -> (" << XX(predicted_opp_reply_.to) << ","
                 << YY(predicted_opp_reply_.to) << ")");
    } else {
        LOG_INFO("SHORTCUT prediction: none");
    }
    const int red_count = count_bits(red_);
    const int blue_count = count_bits(blue_);
    const int occupied = red_count + blue_count;
    const int playable = 64 - count_bits(holes_);
    const int pct = occupied_pct(red_, blue_, holes_);
    const char *phase = occupied_phase(red_, blue_, holes_);
    const char *player = player_name(me_);
    const uint64_t nodes = result_.total_nodes.load(std::memory_order_relaxed);
    const int score = result_.best_score.load(std::memory_order_relaxed);

    LOG_INFO("[MM0MOVE]"
             << " algo=" << options_.algo_id
             << " map=" << map_name_
             << " phase=my_turn"
             << " occupiedPhase=" << phase
             << " turn=" << turn_counter_
             << " move=" << (move_counter_ + 1)
             << " occupied=" << occupied
             << " playable=" << playable
             << " occupiedPct=" << pct
             << " red=" << red_count
             << " blue=" << blue_count
             << " player=" << player
             << " from=" << from
             << " to=" << to
             << " depth=" << depth
             << " nodes=" << nodes
             << " score=" << score
             << " shortcut=" << shortcut_tag);

    std::ostringstream move_line;
    move_line << options_.algo_id << ';'
              << options_.algo_name << ';'
              << map_name_ << ';'
              << "my_turn" << ';'
              << phase << ';'
              << turn_counter_ << ';'
              << (move_counter_ + 1) << ';'
              << occupied << ';'
              << playable << ';'
              << pct << ';'
              << red_count << ';'
              << blue_count << ';'
              << player << ';'
              << from << ';'
              << to << ';'
              << depth << ';'
              << nodes << ';'
              << score << ';'
              << shortcut_tag;
    append_csv_line(options_, "moves",
                    "AlgoId;AlgoName;Map;Phase;OccupiedPhase;TurnIndex;MoveIndex;"
                    "Occupied;Playable;OccupiedPct;Red;Blue;Player;From;To;"
                    "Depth;Nodes;Score;Shortcut",
                    move_line.str());
#endif

    client_->send_move(m);
    pending_shortcut_tag_ = "none";
    shortcut_depth_at_hit_ = -1;
    state_ = State::WAIT_CONFIRMATION;
}

void Engine::apply_opp_move_and_resume(Move m) {
    const bool shortcut_was_active = shortcut_search_active_;
    const bool shortcut_hit = shortcut_was_active && (m == shortcut_opp_move_);

    if (shortcut_hit) {
        ++shortcut_hits_;
        shortcut_depth_at_hit_ = result_.depth_reached.load(std::memory_order_relaxed);

        LOG_INFO_NONFULLSPEED("SHORTCUT hit: keeping search alive for MY_TURN"
                              << " hit=" << shortcut_hits_
                              << " miss=" << shortcut_misses_);

        apply_move_local(m.from, m.to);
        shortcut_search_active_ = false;
        shortcut_opp_move_ = Move();
        predicted_opp_reply_ = Move();
        pending_shortcut_tag_ = "hit";

        if (turn_ == me_ && can_move_local(me_)) {
            ++turn_counter_;
            LOG_INFO_NONFULLSPEED("--- MY_TURN " << turn_counter_ << " (shortcut carry) ---");
            state_ = State::MY_TURN;
            clock_.arm(config::THINK_TIME_MS);
            return;
        }

        // Fallback
        stop_and_join_search();
        dispatch_current_turn();
        return;
    }

    if (shortcut_search_active_) {
        ++shortcut_misses_;
        pending_shortcut_tag_ = "miss";
        LOG_INFO_NONFULLSPEED("SHORTCUT miss: predicted ("
                              << XX(shortcut_opp_move_.from) << "," << YY(shortcut_opp_move_.from)
                              << ") -> (" << XX(shortcut_opp_move_.to) << ","
                              << YY(shortcut_opp_move_.to) << "), actual ("
                              << XX(m.from) << "," << YY(m.from) << ") -> ("
                              << XX(m.to) << "," << YY(m.to) << ")"
                              << " hit=" << shortcut_hits_
                              << " miss=" << shortcut_misses_);
    }

    stop_and_join_search();
    invalidate_shortcut();
    if (shortcut_was_active && pending_shortcut_tag_ == "none") {
        pending_shortcut_tag_ = "miss";
    }

    apply_move_local(m.from, m.to);

    dispatch_current_turn();
}

void Engine::launch_search(Player side_to_move, Player perspective) {
    launch_search_on(red_, blue_, side_to_move, perspective);
}

void Engine::launch_search_on(Bitboard red, Bitboard blue, Player side_to_move, Player perspective) {

    stop_and_join_search();

    result_.reset();
    search_stop_.store(false, std::memory_order_relaxed);

    const Bitboard snap_red = red;
    const Bitboard snap_blue = blue;
    const Bitboard snap_holes = holes_;
    const Player snap_turn = side_to_move;
    const Player snap_perspective = perspective;
    const int snap_turn_index =
        (side_to_move == me_ && state_ == State::OPP_TURN) ? (turn_counter_ + 1) : turn_counter_;

    strategy::SearchContext ctx;
    ctx.algo_id = options_.algo_id;
    ctx.algo_name = options_.algo_name;
    ctx.map_name = map_name_;
    ctx.log_dir = options_.log_dir;
    ctx.log_files = options_.log_files;
    ctx.phase = (side_to_move == me_) ? "my_turn" : "opp_turn";
    ctx.turn_index = snap_turn_index;
    ctx.occupied = count_bits(snap_red) + count_bits(snap_blue);
    ctx.playable = 64 - count_bits(snap_holes);
    ctx.occupied_pct = occupied_pct(snap_red, snap_blue, snap_holes);
    ctx.occupied_phase = occupied_phase(snap_red, snap_blue, snap_holes);
    ctx.red = count_bits(snap_red);
    ctx.blue = count_bits(snap_blue);
    ctx.perspective = player_name(perspective);
    ctx.root_turn = player_name(side_to_move);

    search_thread_ = std::thread([this, snap_red, snap_blue, snap_holes,
                                  snap_turn, snap_perspective, ctx] {
        strategy::compute_best_move(snap_holes, snap_red, snap_blue,
                                    snap_turn, snap_perspective,
                                    search_stop_, result_, ctx);
    });
}

void Engine::stop_and_join_search() {
    search_stop_.store(true, std::memory_order_relaxed);
    if (search_thread_.joinable()) {
        search_thread_.join();
    }
    shortcut_search_active_ = false;
    shortcut_opp_move_ = Move();
}

void Engine::apply_move_local(int from, int to) {
    if (from < 0 || from >= 64 || to < 0 || to >= 64) {
        LOG_INFO_NONFULLSPEED("apply_move_local: invalid move from=" << from << " to=" << to);
        return;
    }

    apply_move_on(red_, blue_, turn_, from, to);
    ++move_counter_;
}

bool Engine::can_move_local(Player player) const {
    return can_move_on(red_, blue_, holes_, player);
}

bool Engine::is_legal_move_local(Player player, Move m) const {
    return is_legal_on(red_, blue_, holes_, player, m);
}

bool Engine::try_launch_shortcut_search() {
    if (!config::SHORTCUT_ENABLED)
        return false;

    if (!predicted_opp_reply_.is_valid()) {
        ++shortcut_no_prediction_;
        LOG_INFO_NONFULLSPEED("SHORTCUT unavailable: no predicted opponent reply"
                              << " none=" << shortcut_no_prediction_);
        return false;
    }

    if (!is_legal_move_local(turn_, predicted_opp_reply_)) {
        ++shortcut_illegal_;
        LOG_INFO_NONFULLSPEED("SHORTCUT unavailable: predicted reply is illegal now ("
                              << XX(predicted_opp_reply_.from) << ","
                              << YY(predicted_opp_reply_.from) << ") -> ("
                              << XX(predicted_opp_reply_.to) << ","
                              << YY(predicted_opp_reply_.to) << ")"
                              << " illegal=" << shortcut_illegal_);
        predicted_opp_reply_ = Move();
        return false;
    }

    Bitboard spec_red = red_;
    Bitboard spec_blue = blue_;
    Player spec_turn = turn_;
    Move assumed = predicted_opp_reply_;
    apply_move_on(spec_red, spec_blue, spec_turn, assumed.from, assumed.to);

    if (spec_turn != me_ || !can_move_on(spec_red, spec_blue, holes_, me_)) {
        ++shortcut_illegal_;
        LOG_INFO_NONFULLSPEED("SHORTCUT unavailable: predicted board does not give us a legal turn"
                              << " illegal=" << shortcut_illegal_);
        predicted_opp_reply_ = Move();
        return false;
    }

    launch_search_on(spec_red, spec_blue, spec_turn, me_);
    shortcut_opp_move_ = assumed;
    shortcut_search_active_ = true;
    predicted_opp_reply_ = Move();

    LOG_INFO_NONFULLSPEED("SHORTCUT launched: assuming OPP ("
                          << XX(assumed.from) << "," << YY(assumed.from)
                          << ") -> (" << XX(assumed.to) << "," << YY(assumed.to)
                          << "), searching our next turn already");
    return true;
}

Move Engine::read_predicted_opp_reply() const {
    int from = result_.reply_from.load(std::memory_order_relaxed);
    int to = result_.reply_to.load(std::memory_order_relaxed);
    if (from < 0 || to < 0)
        return Move();
    return Move(from, to);
}

void Engine::invalidate_shortcut() {
    predicted_opp_reply_ = Move();
    shortcut_opp_move_ = Move();
    shortcut_search_active_ = false;
    shortcut_depth_at_hit_ = -1;
    pending_shortcut_tag_ = "none";
}

void Engine::maybe_send_pregame_commands() {
    if (!client_->is_connected() || state_ != State::IDLE)
        return;

    if (options_.map_set && !map_command_sent_) {
        client_->send_map(options_.map);
        map_name_ = options_.map;
        map_command_sent_ = true;
        next_start_attempt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
        return;
    }

    if (!options_.auto_start || start_command_sent_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (next_start_attempt_.time_since_epoch().count() != 0 && now < next_start_attempt_)
        return;

    client_->send_start();
    start_command_sent_ = true;
}

int Engine::count_blobs(Player player) const {
    return count_bits((player == RED) ? red_ : blue_);
}

void Engine::log_final_score(const char *reason) {
#ifdef MM0_FULLSPEED_BUILD
    (void)reason;
    return;
#else
    const int red_count = count_blobs(RED);
    const int blue_count = count_blobs(BLUE);
    const int occupied = red_count + blue_count;
    const int playable = 64 - count_bits(holes_);
    const int pct = occupied_pct(red_, blue_, holes_);
    const char *phase = occupied_phase(red_, blue_, holes_);
    const char *player = player_name(me_);
    const char *outcome = (red_count > blue_count)   ? "RED_win"
                          : (blue_count > red_count) ? "BLUE_win"
                                                     : "DRAW";

    LOG_INFO("[MM0GAME]"
             << " event=final"
             << " algo=" << options_.algo_id
             << " map=" << map_name_
             << " reason=" << (reason ? reason : "unknown")
             << " player=" << player
             << " red=" << red_count
             << " blue=" << blue_count
             << " occupied=" << occupied
             << " playable=" << playable
             << " occupiedPct=" << pct
             << " occupiedPhase=" << phase
             << " moves=" << move_counter_
             << " outcome=" << outcome
             << " shortcutHit=" << shortcut_hits_
             << " shortcutMiss=" << shortcut_misses_
             << " shortcutNone=" << shortcut_no_prediction_
             << " shortcutIllegal=" << shortcut_illegal_);

    std::ostringstream line;
    line << options_.algo_id << ';'
         << options_.algo_name << ';'
         << map_name_ << ';'
         << (reason ? reason : "unknown") << ';'
         << player << ';'
         << red_count << ';'
         << blue_count << ';'
         << occupied << ';'
         << playable << ';'
         << pct << ';'
         << phase << ';'
         << move_counter_ << ';'
         << outcome;
    append_csv_line(options_, "games",
                    "AlgoId;AlgoName;Map;Reason;Player;Red;Blue;Occupied;"
                    "Playable;OccupiedPct;OccupiedPhase;Moves;Outcome",
                    line.str());
#endif
}

void Engine::dispatch_current_turn() {
    const bool red_can = can_move_local(RED);
    const bool blue_can = can_move_local(BLUE);

    if (!red_can && !blue_can) {
        LOG_INFO_NONFULLSPEED("No legal move left locally; waiting for server close");
        LOG_INFO_NONFULLSPEED("SHORTCUT stats: hit=" << shortcut_hits_
                                                     << " miss=" << shortcut_misses_
                                                     << " none=" << shortcut_no_prediction_
                                                     << " illegal=" << shortcut_illegal_);
        state_ = State::GAME_OVER;
        log_final_score("no_legal_move");
        clock_.cancel();
        stop_and_join_search();
        return;
    }

    if (!can_move_local(turn_)) {
        LOG_INFO_NONFULLSPEED("Local skip: " << player_name(turn_) << " cannot move");
        turn_ = opponent(turn_);
    }

    if (turn_ == me_) {
        enter_my_turn();
    } else {
        enter_opp_turn();
    }
}

}