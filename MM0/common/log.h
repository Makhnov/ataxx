#ifndef MM0_COMMON_LOG_H
#define MM0_COMMON_LOG_H

#include <iostream>
#include <mutex>

namespace mm0::log_util {
extern std::mutex g_log_mtx;
}

// Logs centraux
#define LOG_INFO(msg)                                                \
    do {                                                             \
        std::lock_guard<std::mutex> _lk(::mm0::log_util::g_log_mtx); \
        std::cerr << "[INFO] " << msg << std::endl;                  \
    } while (0)

// Compilation pour tablebases
#ifdef MM0_FULLSPEED_BUILD
#define LOG_INFO_NONFULLSPEED(msg) \
    do {                           \
    } while (0)
#else
#define LOG_INFO_NONFULLSPEED(msg) LOG_INFO(msg)
#endif

// Logs de debug, désactivés en VF
#ifdef DEBUG
#define LOG_DEBUG(msg)                                               \
    do {                                                             \
        std::lock_guard<std::mutex> _lk(::mm0::log_util::g_log_mtx); \
        std::cerr << "[DEBUG] " << msg << std::endl;                 \
    } while (0)
#else
#define LOG_DEBUG(msg) \
    do {               \
    } while (0)
#endif

#endif
