# Legacy strategy framework

This directory restores the historical Strategy/bidiarray/movement framework needed by the early Blobwar strategies.

Provenance:
- source repository: Makhnov/blobmap
- source commit: 70e86dcf209d406c6344cca0b89ce4a00ef0cbf8
- imported source files are unchanged except `strategy.h`, which adds declarations required by the current `strategy_minimax.cc`
- `strategy_compat.cc` provides the missing `isEndGame()` member using the end-game logic already present in `strategies/strategy_minimax_par.cc`

The strategy implementations themselves remain in `../strategies/`.

Build:
- `make greedy`
- `make minimax`
- `make alphabeta`

The resulting `*-core` binaries are the historical strategy computation runners. They are not MM0 network clients. Network/runtime integration is handled separately.

No generated objects or historical executables from blobmap are imported here.
