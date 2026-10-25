#include "rules/board.h"

#include "common/config.h"
#include "common/log.h"

#include <fstream>
#include <string>

namespace mm0 {

bool load_map(const char *mapname, Bitboard &holes_out) {
    std::string path = std::string(config::MAP_DIR) + "/" + mapname;
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_INFO("load_map: unable to open " << path);
        holes_out = 0;
        return false;
    }
    Bitboard h = 0;
    for (int row = 0; row < 8; ++row) {
        std::string line;
        if (!std::getline(f, line)) {
            LOG_INFO("load_map: truncated file " << path);
            holes_out = 0;
            return false;
        }
        for (int col = 0; col < 8 && col < int(line.size()); ++col) {
            if (line[col] == 'x')
                h |= (Bitboard(1) << sq_index(row, col));
        }
    }
    holes_out = h;
    return true;
}

} // namespace mm0
