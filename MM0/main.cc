// MM0 entry point — SUPRAPAR verbatim port with stateful TT.

#include <SDL.h>
#include <SDL_net.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "common/config.h"
#include "common/log.h"
#include "engine/engine.h"
#include "strategy/strategy.h"

namespace {

std::atomic<mm0::Engine *> g_engine{nullptr};

void on_signal(int sig) {
    if (auto *e = g_engine.load(std::memory_order_relaxed)) {
        e->request_shutdown();
    }
    (void)sig;
}

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool parse_bool(const std::string &value, bool &out) {
    const std::string v = lower_copy(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

void print_usage(const char *argv0) {
    std::fprintf(stderr,
                 "Usage: %s [SERVER=host] [PORT=2001] [TEAM=name] [MAP=standard] [START=1] [COLOR=red|blue] [LOG_DIR=../logs/tournament] [LOG_FILES=1]\n"
                 "Notes: the legacy server assigns RED to the first connection and BLUE to the second.\n",
                 argv0);
}

bool parse_options(int argc, char **argv, mm0::config::RuntimeOptions &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "-h" || arg == "--help" || arg == "HELP") {
            print_usage(argv[0]);
            return false;
        }

        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos) {
            LOG_INFO_NONFULLSPEED("Ignoring argument without KEY=value form: " << arg);
            continue;
        }

        const std::string key = lower_copy(arg.substr(0, eq));
        const std::string value = arg.substr(eq + 1);

        if (key == "server" || key == "host") {
            if (!value.empty())
                options.server = value;
        } else if (key == "algo" || key == "algo_id" || key == "algoid") {
            if (!value.empty()) {
                options.algo_id = value;
                if (options.teamname == mm0::config::TEAMNAME) {
                    options.teamname = value;
                }
            }
        } else if (key == "algo_name" || key == "algoname") {
            if (!value.empty())
                options.algo_name = value;
        } else if (key == "port") {
            char *end = nullptr;
            const long port = std::strtol(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == 0 && port > 0 && port <= 65535) {
                options.port = static_cast<uint16_t>(port);
            } else {
                LOG_INFO_NONFULLSPEED("Ignoring invalid PORT=" << value);
            }
        } else if (key == "team" || key == "teamname" || key == "name") {
            if (!value.empty())
                options.teamname = value;
        } else if (key == "log_dir" || key == "logdir") {
            if (!value.empty())
                options.log_dir = value;
        } else if (key == "log_files" || key == "logfiles") {
            bool parsed = options.log_files;
            if (parse_bool(value, parsed)) {
                options.log_files = parsed;
            } else {
                LOG_INFO_NONFULLSPEED("Ignoring invalid " << arg << " (expected 0/1/true/false)");
            }
        } else if (key == "map") {
            if (!value.empty()) {
                options.map = value;
                options.map_set = true;
            }
        } else if (key == "start" || key == "auto_start" || key == "autostart") {
            bool parsed = options.auto_start;
            if (parse_bool(value, parsed)) {
                options.auto_start = parsed;
            } else {
                LOG_INFO_NONFULLSPEED("Ignoring invalid " << arg << " (expected 0/1/true/false)");
            }
        } else if (key == "color" || key == "colour") {
            const std::string color = lower_copy(value);
            if (color == "red" || color == "blue") {
                options.requested_color = color;
                options.color_set = true;
            } else {
                LOG_INFO_NONFULLSPEED("Ignoring invalid COLOR=" << value << " (expected red or blue)");
            }
        } else {
            LOG_INFO_NONFULLSPEED("Ignoring unknown option: " << arg);
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    mm0::config::RuntimeOptions options;
    if (!parse_options(argc, argv, options)) {
        return 0;
    }

    if (SDL_Init(0) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDLNet_Init() == -1) {
        std::fprintf(stderr, "SDLNet_Init failed: %s\n", SDLNet_GetError());
        SDL_Quit();
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

#ifdef _OPENMP
    // Match SUPRAPAR's thread count (10 by default).
    if (mm0::config::OMP_THREADS > 0) {
        omp_set_num_threads(mm0::config::OMP_THREADS);
    }
#endif

    LOG_INFO_NONFULLSPEED("MM0 starting (SUPRAPAR verbatim port, OMP="
                          << omp_get_max_threads() << " threads)");
    LOG_INFO_NONFULLSPEED("Runtime: server=" << options.server << ":" << options.port
                                             << " algo=" << options.algo_id
                                             << " algo_name=\"" << options.algo_name << "\""
                                             << " team=\"" << options.teamname << "\""
                                             << " map=" << (options.map_set ? options.map : "(server default)")
                                             << " auto_start=" << (options.auto_start ? "on" : "off")
                                             << " log_files=" << (options.log_files ? "on" : "off")
                                             << " log_dir=" << options.log_dir
                                             << " color_request="
                                             << (options.color_set ? options.requested_color : "(server assigned)"));

    // One-shot strategy init (precomputes validClones/validJumps + Zobrist).
    mm0::strategy::init();

    {
        mm0::Engine engine(options);
        g_engine.store(&engine, std::memory_order_relaxed);
        engine.run();
        g_engine.store(nullptr, std::memory_order_relaxed);
    }

    SDLNet_Quit();
    SDL_Quit();

    LOG_INFO_NONFULLSPEED("MM0 exited cleanly");
    return 0;
}
