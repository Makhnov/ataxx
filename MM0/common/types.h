#ifndef MM0_COMMON_TYPES_H
#define MM0_COMMON_TYPES_H

#include <SDL.h>
#include <cstdint>

namespace mm0 {

using Bitboard = uint64_t;

enum Player : Uint16 {
    RED = 0,
    BLUE = 1
};

// index = y*8 + x (from == -1 pour un move invalide)
struct Move {
    Sint8 from;
    Sint8 to;

    constexpr Move() : from(-1), to(-1) {}
    constexpr Move(int f, int t) : from(Sint8(f)), to(Sint8(t)) {}

    constexpr bool is_valid() const { return from >= 0 && to >= 0; }
    constexpr bool operator==(const Move &o) const { return from == o.from && to == o.to; }
    constexpr bool operator!=(const Move &o) const { return !(*this == o); }
};

constexpr int sq_index(int x, int y) { return y * 8 + x; }
constexpr int sq_x(int s) { return s & 7; }
constexpr int sq_y(int s) { return s >> 3; }

// Helpers pour manipuler plus facilement les coordonnées dans le client
constexpr Move move_from_coords(int ox, int oy, int nx, int ny) {
    return Move(sq_index(ox, oy), sq_index(nx, ny));
}

constexpr Player opponent(Player p) { return (p == RED) ? BLUE : RED; }

} // namespace mm0

#endif