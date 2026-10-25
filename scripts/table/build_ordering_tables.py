#!/usr/bin/env python3
import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


DEFAULT_STRONG_ALGOS = {
    "NMP_TT_SF_v3D",
    "NMP_TT_SF_v5D",
}

MAPS = ("standard", "x")
PLAYERS = ("RED", "BLUE")

MOVE_SENT_RE = re.compile(
    r'player:\s*(\d+)\s+sent\s+:"/do_move\s+\((\d+),(\d+)\)\s+to\s+\((\d+),(\d+)\)"'
)
MOVE_SUGGESTED_RE = re.compile(
    r"player:\s*(\d+)\s+suggested a move\s*:\s*/do_move\s+\((\d+),(\d+)\)\s+to\s+\((\d+),(\d+)\)"
)
MOVE_ACCEPTED_RE = re.compile(r"move from \((\d+),(\d+)\) to \((\d+),(\d+)\) accepted")
WINNER_RE = re.compile(r"winner:\s*(\d+)")
PLAYER_SCORE_RE = re.compile(r"player\s+([01]):\s*(\d+)")


def sq(x, y):
    return y * 8 + x


def xy(index):
    return [index & 7, index >> 3]


def bit(index):
    return 1 << index


def bit_count(value):
    return int(value).bit_count()


def hex64(value):
    return f"0x{value:016x}"


def move_code(src, dst):
    return (src << 6) | dst


def to_int(value, default=0):
    try:
        if value in (None, ""):
            return default
        return int(float(value))
    except (TypeError, ValueError):
        return default


def parse_hex(value):
    if not value:
        return None
    try:
        return int(str(value), 16)
    except ValueError:
        return None


def phase_name(occupied, playable):
    pct = (occupied * 100) // playable if playable else 100
    if pct < 33:
        return "opening"
    if pct < 66:
        return "midgame"
    if pct < 90:
        return "endgame"
    return "terminal"


def load_holes(root, map_name):
    path = root / "data" / "boards" / map_name
    holes = 0
    for row, line in enumerate(path.read_text().splitlines()[:8]):
        for col, char in enumerate(line[:8]):
            if char == "x":
                holes |= bit(sq(row, col))
    return holes


def initial_board(holes):
    red = bit(sq(0, 0)) | bit(sq(7, 0))
    blue = bit(sq(0, 7)) | bit(sq(7, 7))
    return red & ~holes, blue & ~holes


def apply_move(red, blue, player_id, src, dst):
    me = red if player_id == 0 else blue
    opp = blue if player_id == 0 else red
    sx, sy = xy(src)
    dx, dy = xy(dst)

    if max(abs(dx - sx), abs(dy - sy)) == 2:
        me &= ~bit(src)
    me |= bit(dst)

    flips = 0
    for nx in range(max(0, dx - 1), min(7, dx + 1) + 1):
        for ny in range(max(0, dy - 1), min(7, dy + 1) + 1):
            flips |= bit(sq(nx, ny))
    flips &= opp
    opp &= ~flips
    me |= flips

    if player_id == 0:
        return me, opp
    return opp, me


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def read_semicolon_csv(path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f, delimiter=";"))


def parse_winner(server_log):
    text = server_log.read_text(errors="replace") if server_log.exists() else ""
    winner_match = None
    for match in WINNER_RE.finditer(text):
        winner_match = match
    if winner_match:
        return int(winner_match.group(1))

    scores = {}
    for match in PLAYER_SCORE_RE.finditer(text):
        scores[int(match.group(1))] = int(match.group(2))
    if 0 in scores and 1 in scores and scores[0] != scores[1]:
        return 0 if scores[0] > scores[1] else 1
    return None


def parse_accepted_moves(server_log):
    moves = []
    pending = None
    if not server_log.exists():
        return moves

    for line in server_log.read_text(errors="replace").splitlines():
        match = MOVE_SENT_RE.search(line) or MOVE_SUGGESTED_RE.search(line)
        if match:
            player = int(match.group(1))
            coords = tuple(int(match.group(i)) for i in range(2, 6))
            pending = (player, coords)
            continue

        match = MOVE_ACCEPTED_RE.search(line)
        if not match:
            continue

        coords = tuple(int(match.group(i)) for i in range(1, 5))
        player = pending[0] if pending and pending[1] == coords else None
        if player is None:
            pending = None
            continue
        moves.append((player, sq(coords[0], coords[1]), sq(coords[2], coords[3])))
        pending = None

    return moves


def pop_matching_move_row(rows, cursor, src, dst):
    for idx in range(cursor, min(len(rows), cursor + 4)):
        row = rows[idx]
        if to_int(row.get("From"), -1) == src and to_int(row.get("To"), -1) == dst:
            return row, idx + 1
    # Do not consume an arbitrary row: its TurnIndex could hide a valid
    # root_ranking match that is already protected by exact bitboards.
    return {}, cursor


def position_key(map_name, player_name, red, blue):
    return f"{map_name}:{player_name}:{red:016x}:{blue:016x}"


def empty_entry():
    return {
        "map": "",
        "player": "",
        "red": "",
        "blue": "",
        "holes": "",
        "occupied": 0,
        "phase": "",
        "visits": 0,
        "total_weight": 0,
        "winner_algos": defaultdict(int),
        "moves": defaultdict(lambda: {
            "from": -1,
            "to": -1,
            "visits": 0,
            "weight": 0,
            "rank_sum": 0,
            "depth_sum": 0,
            "max_depth": 0,
            "nodes_sum": 0,
            "score_sum": 0,
        }),
    }


def add_ranked_position(table, map_name, player_name, holes, red, blue,
                        ranked_rows, winner_algo, top_k):
    key = position_key(map_name, player_name, red, blue)
    occupied = bit_count(red | blue)
    playable = 64 - bit_count(holes)

    entry = table[key]
    entry["map"] = map_name
    entry["player"] = player_name
    entry["red"] = hex64(red)
    entry["blue"] = hex64(blue)
    entry["holes"] = hex64(holes)
    entry["occupied"] = occupied
    entry["phase"] = phase_name(occupied, playable)
    entry["visits"] += 1
    entry["winner_algos"][winner_algo] += 1

    for row in ranked_rows[:top_k]:
        src = to_int(row.get("From"), -1)
        dst = to_int(row.get("To"), -1)
        rank = max(1, to_int(row.get("Rank"), 1))
        depth = to_int(row.get("Depth"), 0)
        nodes = to_int(row.get("Nodes"), 0)
        score = to_int(row.get("Score"), 0)
        if src < 0 or dst < 0:
            continue

        weight = max(1, depth * depth) * max(1, top_k - rank + 1)
        move = entry["moves"][move_code(src, dst)]
        move["from"] = src
        move["to"] = dst
        move["visits"] += 1
        move["weight"] += weight
        move["rank_sum"] += rank
        move["depth_sum"] += depth
        move["max_depth"] = max(move["max_depth"], depth)
        move["nodes_sum"] += nodes
        move["score_sum"] += score
        entry["total_weight"] += weight


def best_root_ranking_rows(rows, map_name, player_name, holes, red, blue, move_row):
    turn_index = move_row.get("TurnIndex")
    candidates = []
    for row in rows:
        if row.get("Phase") != "my_turn":
            continue
        if row.get("Map") != map_name or row.get("RootTurn") != player_name:
            continue
        if turn_index not in (None, "") and row.get("TurnIndex") != turn_index:
            continue
        if parse_hex(row.get("RedBits")) != red:
            continue
        if parse_hex(row.get("BlueBits")) != blue:
            continue
        if parse_hex(row.get("Holes")) != holes:
            continue
        candidates.append(row)

    if not candidates:
        return []

    best_depth = max(to_int(row.get("Depth"), 0) for row in candidates)
    same_depth = [row for row in candidates if to_int(row.get("Depth"), 0) == best_depth]
    same_depth.sort(key=lambda row: to_int(row.get("Rank"), 99))
    return same_depth


def fallback_actual_move_row(src, dst, move_row):
    return {
        "Rank": "1",
        "From": str(src),
        "To": str(dst),
        "Depth": move_row.get("Depth", "0"),
        "Nodes": move_row.get("Nodes", "0"),
        "Score": move_row.get("Score", "0"),
    }


def finalize_entry(key, entry, top_k):
    moves = sorted(
        entry["moves"].values(),
        key=lambda move: (
            -move["weight"],
            -move["visits"],
            move["rank_sum"],
            -move["max_depth"],
            move["from"],
            move["to"],
        ),
    )
    total_weight = entry["total_weight"]
    best_weight = moves[0]["weight"] if moves else 0
    out_moves = []
    for move in moves[:top_k]:
        visits = move["visits"]
        out_moves.append({
            "from": move["from"],
            "to": move["to"],
            "from_xy": xy(move["from"]),
            "to_xy": xy(move["to"]),
            "visits": visits,
            "weight": move["weight"],
            "avg_rank": round(move["rank_sum"] / visits, 3) if visits else 0,
            "avg_depth": round(move["depth_sum"] / visits, 3) if visits else 0,
            "max_depth": move["max_depth"],
            "avg_nodes": round(move["nodes_sum"] / visits, 3) if visits else 0,
            "avg_score": round(move["score_sum"] / visits, 3) if visits else 0,
        })

    return {
        "key": key,
        "map": entry["map"],
        "player": entry["player"],
        "red": entry["red"],
        "blue": entry["blue"],
        "holes": entry["holes"],
        "occupied": entry["occupied"],
        "phase": entry["phase"],
        "visits": entry["visits"],
        "total_weight": total_weight,
        "confidence": round(best_weight / total_weight, 6) if total_weight else 0,
        "winner_algos": dict(sorted(entry["winner_algos"].items())),
        "moves": out_moves,
    }


def build_tables(root, strong_algos, top_k, allow_fallback):
    tables = {
        (map_name, player): defaultdict(empty_entry)
        for map_name in MAPS
        for player in PLAYERS
    }
    counters = defaultdict(int)
    holes_by_map = {map_name: load_holes(root, map_name) for map_name in MAPS}

    for meta_path in sorted((root / "logs" / "tournament" / "runs").glob("*/match_*/meta.json")):
        match_dir = meta_path.parent
        meta = read_json(meta_path)
        if not meta:
            counters["bad_meta"] += 1
            continue
        map_name = meta.get("map")
        if map_name not in MAPS:
            counters["ignored_map"] += 1
            continue

        winner = parse_winner(match_dir / "server.log")
        if winner is None:
            counters["draw_or_unknown"] += 1
            continue

        winner_algo = meta["redAlgo"] if winner == 0 else meta["blueAlgo"]
        if winner_algo not in strong_algos:
            counters["weak_winner"] += 1
            continue

        moves = parse_accepted_moves(match_dir / "server.log")
        if not moves:
            counters["no_moves"] += 1
            continue

        algos = {meta["redAlgo"], meta["blueAlgo"]}
        move_rows = {
            algo: [row for row in read_semicolon_csv(match_dir / "raw" / f"{algo}.moves.csv")
                   if row.get("Phase") == "my_turn"]
            for algo in algos
        }
        ranking_rows = {
            algo: read_semicolon_csv(match_dir / "raw" / f"{algo}.root_ranking.csv")
            for algo in algos
        }
        cursors = {algo: 0 for algo in algos}
        holes = holes_by_map[map_name]
        red, blue = initial_board(holes)

        for player, src, dst in moves:
            algo = meta["redAlgo"] if player == 0 else meta["blueAlgo"]
            move_row, cursors[algo] = pop_matching_move_row(
                move_rows.get(algo, []),
                cursors.get(algo, 0),
                src,
                dst,
            )

            if player == winner:
                player_name = "RED" if player == 0 else "BLUE"
                ranked = best_root_ranking_rows(
                    ranking_rows.get(algo, []),
                    map_name,
                    player_name,
                    holes,
                    red,
                    blue,
                    move_row,
                )
                if ranked:
                    counters["ranking_observations"] += 1
                elif allow_fallback:
                    counters["fallback_actual_move"] += 1
                    ranked = [fallback_actual_move_row(src, dst, move_row)]
                else:
                    counters["missing_root_ranking"] += 1
                    red, blue = apply_move(red, blue, player, src, dst)
                    continue

                add_ranked_position(
                    tables[(map_name, player_name)],
                    map_name,
                    player_name,
                    holes,
                    red,
                    blue,
                    ranked,
                    winner_algo,
                    top_k,
                )
                counters["observations"] += 1

            red, blue = apply_move(red, blue, player, src, dst)
        counters["games_used"] += 1

    return tables, counters


def write_table(path, table, map_name, player, counters, strong_algos, top_k):
    entries = [finalize_entry(key, entry, top_k) for key, entry in table.items()]
    entries.sort(key=lambda entry: (
        entry["occupied"],
        -entry["visits"],
        -entry["confidence"],
        entry["key"],
    ))
    payload = {
        "schema": "blobwar_static_ordering_v2_root_ranking",
        "map": map_name,
        "player": player,
        "strong_algos": sorted(strong_algos),
        "top_k": top_k,
        "entry_count": len(entries),
        "source": "logs/tournament/runs",
        "counters": dict(sorted(counters.items())),
        "entries": entries,
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))


def display_path(path, root):
    try:
        return path.relative_to(root)
    except ValueError:
        return path


def main():
    parser = argparse.ArgumentParser(
        description="Build static move-ordering JSON tables from winning games and Deep root rankings."
    )
    parser.add_argument("--root", default=".", help="Repository root")
    parser.add_argument(
        "--out-dir",
        default="scripts/table/generated",
        help="Output directory for the four JSON tables",
    )
    parser.add_argument("--top-k", type=int, default=3, help="Moves stored per position")
    parser.add_argument(
        "--strong-algo",
        action="append",
        dest="strong_algos",
        help="Algo id allowed as a table source. Repeatable.",
    )
    parser.add_argument(
        "--deep-only",
        action="store_true",
        help="Deprecated no-op: defaults already use only Deep algo ids.",
    )
    parser.add_argument(
        "--allow-fallback-actual-move",
        action="store_true",
        help="If root_ranking.csv is missing, fall back to the played move as rank 1.",
    )
    parser.add_argument(
        "--include-legacy-strong",
        action="store_true",
        help="Also include pre-Deep strong algos; use with --allow-fallback-actual-move for old logs.",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    strong_algos = set(args.strong_algos or DEFAULT_STRONG_ALGOS)
    if args.include_legacy_strong and not args.strong_algos:
        strong_algos.update({
            "NMP_TT_v2",
            "NMP_TT_SF_v1",
            "NMP_TT_SF_v3",
            "NMP_TT_SF_v5",
        })

    out_dir = (root / args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    tables, counters = build_tables(
        root,
        strong_algos,
        args.top_k,
        args.allow_fallback_actual_move,
    )
    for map_name in MAPS:
        for player in PLAYERS:
            out_path = out_dir / f"ordering_{map_name}_{player.lower()}.json"
            write_table(
                out_path,
                tables[(map_name, player)],
                map_name,
                player,
                counters,
                strong_algos,
                args.top_k,
            )
            print(f"wrote {display_path(out_path, root)}")

    print("summary:")
    for key, value in sorted(counters.items()):
        print(f"  {key}: {value}")


if __name__ == "__main__":
    main()
