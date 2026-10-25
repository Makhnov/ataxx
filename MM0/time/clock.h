#ifndef MM0_TIME_CLOCK_H
#define MM0_TIME_CLOCK_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace mm0 {

class Clock {
public:
    Clock();
    ~Clock();

    void arm(int ms);

    void force_stop();

    bool stopped() const { return stop_.load(std::memory_order_relaxed); }

    void cancel();

    std::atomic<bool> &stop_flag() { return stop_; }

private:
    void watchdog_loop();

    std::atomic<bool> stop_{true};      // Pas de recherche en cours
    std::atomic<bool> shutdown_{false}; // Demande d'arrêt de l'horloge
    std::chrono::steady_clock::time_point deadline_{};
    std::thread watchdog_;
};

} // namespace mm0

#endif
