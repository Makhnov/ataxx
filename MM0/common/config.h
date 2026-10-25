#ifndef MM0_COMMON_CONFIG_H
#define MM0_COMMON_CONFIG_H

#include <cstdint>
#include <string>

namespace mm0::config {

// Données relatives à la version de l'algo
#ifdef MM0_DEEP_BUILD
inline constexpr const char *ALGO_ID = "NMP_TT_SF_v5D";
inline constexpr const char *ALGO_NAME = "stateful-cheatcut-deep";
#elif defined(MM0_FULLSPEED_BUILD)
inline constexpr const char *ALGO_ID = "NMP_TT_SF_v5F";
inline constexpr const char *ALGO_NAME = "stateful-cheatcut-fullspeed";
#else
inline constexpr const char *ALGO_ID = "NMP_TT_SF_v5";
inline constexpr const char *ALGO_NAME = "stateful-cheatcut";
#endif

// Remplacement de l'ancien fichier network.cfg
inline constexpr const char *SERVER = "localhost";
inline constexpr uint16_t PORT = 2001;
inline constexpr const char *TEAMNAME = ALGO_ID;
#ifdef MM0_FULLSPEED_BUILD
inline constexpr bool DEFAULT_LOG_FILES = false;
#else
inline constexpr bool DEFAULT_LOG_FILES = true;
#endif

struct RuntimeOptions {
    std::string server = SERVER;
    uint16_t port = PORT;
    std::string teamname = TEAMNAME;
    std::string algo_id = ALGO_ID;
    std::string algo_name = ALGO_NAME;

    std::string map = "standard";
    bool map_set = false;
    bool auto_start = true;
    std::string requested_color;
    bool color_set = false;

    std::string log_dir = "../logs/tournament";
    bool log_files = DEFAULT_LOG_FILES;
};

// Réflexion à 20s pour populate les tablebases
#ifndef MM0_DEEP_THINK_TIME_MS
#define MM0_DEEP_THINK_TIME_MS 20000
#endif

#ifdef MM0_DEEP_BUILD
inline constexpr int THINK_TIME_MS = MM0_DEEP_THINK_TIME_MS;
#else
inline constexpr int THINK_TIME_MS = 1000;
#endif

// Activation du shortcut
inline constexpr bool SHORTCUT_ENABLED = true;

// Fallback (quand on ne trouve rien)
inline constexpr bool SHORTCUT_FALLBACK_OPP_SEARCH = true;

// Règles du tournoi, 12 threads max
inline constexpr int OMP_THREADS = 12;

// Stockage des tablebases et des fichiers de log
inline constexpr const char *MAP_DIR = "../data/boards";
inline constexpr const char *ORDER_TABLE_DIR = "../tablebase";
inline constexpr int STATIC_ORDER_MAX_PLY = 6;

// Logs plus fin, pour avoir une depth par phase de jeu
inline constexpr int PHASE_OPENING_MAX_PCT = 33;
inline constexpr int PHASE_MIDGAME_MAX_PCT = 66;
inline constexpr int PHASE_ENDGAME_MAX_PCT = 90; // On supprime les logs dégénérés de fin de partie (> 90% occupé)

} // namespace mm0::config

#endif
