#include "time/clock.h"

#include "common/log.h"

namespace mm0 {

Clock::Clock() {
}

Clock::~Clock() {
    cancel();
}

void Clock::arm(int ms) {
    // Au cas où on armerait une horloge déjà active
    cancel();

    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    shutdown_.store(false, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_relaxed);

    watchdog_ = std::thread([this] { watchdog_loop(); });
}

void Clock::force_stop() {
    stop_.store(true, std::memory_order_relaxed);
}

void Clock::cancel() {
    shutdown_.store(true, std::memory_order_relaxed);
    stop_.store(true, std::memory_order_relaxed);
    if (watchdog_.joinable()) {
        watchdog_.join();
    }
}

void Clock::watchdog_loop() {

    // On vérifie régulièrement le shutdown_ pour pouvoir sortir proprement avant le temps écoulé si besoin (et éviter de "terminer" la seconde)
    while (!shutdown_.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline_) {
            stop_.store(true, std::memory_order_relaxed);
            return;
        }
        auto slice = std::min<std::chrono::steady_clock::duration>(
            deadline_ - now,
            std::chrono::milliseconds(5));
        std::this_thread::sleep_for(slice);
    }
}

} // namespace mm0
