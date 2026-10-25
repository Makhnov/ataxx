#ifndef MM0_RULES_BOARD_H
#define MM0_RULES_BOARD_H

#include "common/types.h"

namespace mm0 {

bool load_map(const char *mapname, Bitboard &holes_out);

}

#endif
