#!/usr/bin/env python3
import argparse
import csv
import json
import math
import re
from pathlib import Path


CORE_PHASES = {"opening", "midgame", "endgame"}
DISPLAY_PHASES = ["opening", "midgame", "endgame"]
ALL_PHASES = DISPLAY_PHASES + ["terminal"]


def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def read_semicolon_csv(path: Path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f, delimiter=";"))


def to_int(value, default=0):
    try:
        if value is None or value == "":
            return default
        return int(float(value))
    except (TypeError, ValueError):
        return default


def to_float(value, default=0.0):
    try:
        if value is None or value == "":
            return default
        return float(value)
    except (TypeError, ValueError):
        return default


def median(values):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return 0
    mid = len(vals) // 2
    if len(vals) % 2:
        return vals[mid]
    return (vals[mid - 1] + vals[mid]) / 2


def percentile(values, p):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return 0
    if len(vals) == 1:
        return vals[0]
    idx = (len(vals) - 1) * p
    lo = math.floor(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return vals[lo]
    return vals[lo] * (hi - idx) + vals[hi] * (idx - lo)


def mean(values):
    vals = [v for v in values if v is not None]
    return sum(vals) / len(vals) if vals else 0


def compact_number(value):
    if isinstance(value, float) and value.is_integer():
        return int(value)
    return value


def last_search_rows(search_rows):
    best = {}
    for idx, row in enumerate(search_rows):
        if row.get("Phase") != "my_turn":
            continue
        key = row.get("TurnIndex")
        depth = to_int(row.get("Depth"))
        old = best.get(key)
        if old is None or depth > to_int(old[0].get("Depth")) or (
            depth == to_int(old[0].get("Depth")) and idx > old[1]
        ):
            best[key] = (row, idx)
    return [row for row, _ in best.values()]


def phase_from_row(row):
    return row.get("OccupiedPhase") or "terminal"


def metric_rows(rows, phases):
    return [r for r in rows if phase_from_row(r) in phases]


def pct(num, den):
    return (100.0 * num / den) if den else 0


def sum_col(rows, key):
    return sum(to_int(r.get(key)) for r in rows)


ORDER_SOURCES = [
    ("TTMove", "ttMove"),
    ("Memory", "memory"),
    ("Killer", "killer"),
    ("History", "history"),
]


def tt_metrics(searches, suffix=""):
    probes = sum_col(searches, "TTProbes")
    hits = sum_col(searches, "TTHits")
    cutoffs = sum_col(searches, "TTCutoffs")
    move_hits = sum_col(searches, "TTMoveHits")
    legal_moves = sum_col(searches, "TTLegalMoves")

    hit_rate = pct(hits, probes)
    cutoff_rate = pct(cutoffs, probes)

    metrics = {
        f"ttHitRate{suffix}": hit_rate,
        f"ttCutoffRate{suffix}": cutoff_rate,
        f"ttCutoffPerHit{suffix}": pct(cutoffs, hits),
        f"ttMoveAvailability{suffix}": pct(move_hits, probes),
        f"ttLegalMoveAvailability{suffix}": pct(legal_moves, probes),
        f"_ttProbes{suffix}": probes,
        f"_ttHits{suffix}": hits,
        f"_ttCutoffs{suffix}": cutoffs,
        f"_ttMoveHits{suffix}": move_hits,
        f"_ttLegalMoves{suffix}": legal_moves,
    }
    if suffix == "Core":
        metrics["meanHitRateCore"] = hit_rate
        metrics["meanCutoffRateCore"] = cutoff_rate
    elif suffix == "":
        metrics["meanHitRate"] = hit_rate
        metrics["meanCutoffRate"] = cutoff_rate
    return metrics


def order_metrics(searches, suffix=""):
    tried_moves = sum_col(searches, "OrderTriedMoves")
    cutoffs = sum_col(searches, "OrderCutoffs")
    first_cutoffs = sum_col(searches, "OrderFirstCutoffs")
    cut_index_sum = sum_col(searches, "OrderCutIndexSum")
    root_iterations = sum_col(searches, "RootIterations")
    root_first_best = sum_col(searches, "RootFirstBest")
    root_best_rank_sum = sum_col(searches, "RootBestRankSum")
    root_bound_updates = sum_col(searches, "RootBoundUpdates")
    root_move_count_sum = sum_col(searches, "RootMoveCountSum")

    metrics = {
        f"orderTriedMoves{suffix}": tried_moves,
        f"orderCutoffs{suffix}": cutoffs,
        f"orderCutoffRate{suffix}": pct(cutoffs, tried_moves),
        f"orderFirstCutoffRate{suffix}": pct(first_cutoffs, cutoffs),
        f"orderAvgCutIndex{suffix}": (cut_index_sum / cutoffs) if cutoffs else 0,
        f"rootFirstBestRate{suffix}": pct(root_first_best, root_iterations),
        f"rootAvgBestRank{suffix}": (root_best_rank_sum / root_iterations) if root_iterations else 0,
        f"rootBoundUpdateRate{suffix}": (root_bound_updates / root_iterations) if root_iterations else 0,
        f"rootAvgMoveCount{suffix}": (root_move_count_sum / root_iterations) if root_iterations else 0,
        f"_orderTriedMoves{suffix}": tried_moves,
        f"_orderCutoffs{suffix}": cutoffs,
        f"_orderFirstCutoffs{suffix}": first_cutoffs,
        f"_orderCutIndexSum{suffix}": cut_index_sum,
        f"_rootIterations{suffix}": root_iterations,
        f"_rootFirstBest{suffix}": root_first_best,
        f"_rootBestRankSum{suffix}": root_best_rank_sum,
        f"_rootBoundUpdates{suffix}": root_bound_updates,
        f"_rootMoveCountSum{suffix}": root_move_count_sum,
    }

    impact_total = 0
    for csv_name, _ in ORDER_SOURCES:
        impact_total += sum_col(searches, f"OrderCutDepth{csv_name}")

    for csv_name, key in ORDER_SOURCES:
        tried = sum_col(searches, f"OrderTried{csv_name}")
        source_cutoffs = sum_col(searches, f"OrderCut{csv_name}")
        cut_depth = sum_col(searches, f"OrderCutDepth{csv_name}")
        title = key[0].upper() + key[1:]
        metrics.update({
            f"order{title}Tried{suffix}": tried,
            f"order{title}Cutoffs{suffix}": source_cutoffs,
            f"order{title}CutoffRate{suffix}": pct(source_cutoffs, tried),
            f"order{title}AvgCutDepth{suffix}": (cut_depth / source_cutoffs) if source_cutoffs else 0,
            f"order{title}ImpactShare{suffix}": pct(cut_depth, impact_total),
            f"_order{title}Tried{suffix}": tried,
            f"_order{title}Cutoffs{suffix}": source_cutoffs,
            f"_order{title}CutDepth{suffix}": cut_depth,
        })
    return metrics


def phase_metrics(move_rows, search_rows, phase):
    moves = metric_rows(move_rows, {phase})
    searches = metric_rows(search_rows, {phase})
    metrics = {
        "medianDepth": compact_number(median([to_int(r.get("Depth")) for r in moves])),
        "medianNps": compact_number(median([to_int(r.get("NPS")) for r in searches])),
    }
    metrics.update(tt_metrics(searches))
    metrics.update(order_metrics(searches))
    return metrics


def aggregate_algo(match_dir: Path, algo: str):
    raw = match_dir / "raw"
    move_rows = read_semicolon_csv(raw / f"{algo}.moves.csv")
    search_rows = read_semicolon_csv(raw / f"{algo}.search.csv")

    move_rows = [r for r in move_rows if r.get("Phase") == "my_turn"]
    final_search = last_search_rows(search_rows)
    core_moves = metric_rows(move_rows, CORE_PHASES)
    core_search = metric_rows(final_search, CORE_PHASES)
    terminal_moves = metric_rows(move_rows, {"terminal"})
    all_core_search = [
        r for r in search_rows
        if r.get("Phase") == "my_turn" and phase_from_row(r) in CORE_PHASES
    ]

    shortcut_values = [r.get("Shortcut", "none") for r in move_rows]
    shortcut_hit_values = {"hit", "carry_hit", "useful_hit"}
    shortcut_miss_values = {"miss", "carry_miss"}
    shortcut_hits = sum(1 for v in shortcut_values if v in shortcut_hit_values)
    shortcut_misses = sum(1 for v in shortcut_values if v in shortcut_miss_values)
    shortcut_attempts = shortcut_hits + shortcut_misses
    useful_hits = sum(1 for v in shortcut_values if v == "useful_hit")
    core_tt = tt_metrics(core_search, "Core")
    core_order = order_metrics(core_search, "Core")

    stats = {
        "medianDepthCore": compact_number(median([to_int(r.get("Depth")) for r in core_moves])),
        "p75DepthCore": compact_number(percentile([to_int(r.get("Depth")) for r in core_moves], 0.75)),
        "maxDepthCore": max([to_int(r.get("Depth")) for r in core_moves], default=0),
        "medianNpsCore": compact_number(median([to_int(r.get("NPS")) for r in core_search])),
        "medianNodesCore": compact_number(median([to_int(r.get("Nodes")) for r in core_moves])),
        "totalNodesCore": sum(to_int(r.get("Nodes")) for r in core_moves),
        "maxDepthTerminal": max([to_int(r.get("Depth")) for r in terminal_moves], default=0),
        "medianDepthTerminal": compact_number(median([to_int(r.get("Depth")) for r in terminal_moves])),
        "medianNodesTerminal": compact_number(median([to_int(r.get("Nodes")) for r in terminal_moves])),
        "totalOverwritesCore": sum(to_int(r.get("TTOverwrites")) for r in all_core_search),
        "totalRejectedCore": sum(to_int(r.get("TTRejected")) for r in all_core_search),
        "shortcutAttempts": shortcut_attempts,
        "shortcutHits": shortcut_hits,
        "shortcutMisses": shortcut_misses,
        "shortcutHitRate": (100.0 * shortcut_hits / shortcut_attempts) if shortcut_attempts else 0,
        "shortcutUsefulHits": useful_hits,
        "shortcutUsefulHitRate": (100.0 * useful_hits / shortcut_hits) if shortcut_hits else 0,
        "medianDepthOnShortcutHit": compact_number(median([
            to_int(r.get("Depth")) for r in move_rows
            if r.get("Shortcut") in shortcut_hit_values
        ])),
        "medianDepthOnShortcutMiss": compact_number(median([
            to_int(r.get("Depth")) for r in move_rows
            if r.get("Shortcut") in shortcut_miss_values
        ])),
        "phases": {
            phase: phase_metrics(move_rows, final_search, phase)
            for phase in DISPLAY_PHASES
        },
    }
    stats.update(core_tt)
    stats.update(core_order)
    return stats


def parse_final_from_logs(match_dir: Path):
    forced = read_json(match_dir / "forced_result.json")
    if forced and forced.get("forcedResult") == "draw":
        return {
            "red": to_int(forced.get("red")),
            "blue": to_int(forced.get("blue")),
            "moves": to_int(forced.get("moves")),
            "outcome": "DRAW",
            "source": forced.get("reason", "forced_draw"),
            "forcedResult": "draw",
        }

    server_log = read_text(match_dir / "server.log")
    red = blue = None
    for line in server_log.splitlines():
        m0 = re.search(r"^player 0:\s*(\d+)", line)
        m1 = re.search(r"^player 1:\s*(\d+)", line)
        if m0:
            red = int(m0.group(1))
        if m1:
            blue = int(m1.group(1))

    if red is not None and blue is not None:
        return {
            "red": red,
            "blue": blue,
            "moves": len(re.findall(r"moved from", server_log)),
            "outcome": "RED_win" if red > blue else "BLUE_win" if blue > red else "DRAW",
            "source": "server",
        }
    return None


def is_complete_match(match_dir: Path):
    status = read_text(match_dir / "status.log").strip()
    if not status:
        return False
    if status == "ok" or "draw by repetition" in status:
        return True
    return parse_final_from_logs(match_dir) is not None


def result_from_a_perspective(final, side):
    if final.get("forcedResult") == "draw" or final["red"] == final["blue"]:
        return "draw"
    red_wins = final["red"] > final["blue"]
    if side == "A_red":
        return "A_win" if red_wins else "B_win"
    return "B_win" if red_wins else "A_win"


def build_game(root: Path, match_dir: Path):
    meta = read_json(match_dir / "meta.json")
    if not meta or not is_complete_match(match_dir):
        return None

    final = parse_final_from_logs(match_dir)
    if not final:
        return None

    pair_a = meta["pairA"]
    pair_b = meta["pairB"]
    side = meta["side"]
    red_algo = meta["redAlgo"]
    blue_algo = meta["blueAlgo"]
    score_a = final["red"] if side == "A_red" else final["blue"]
    score_b = final["blue"] if side == "A_red" else final["red"]

    stats_red = aggregate_algo(match_dir, red_algo)
    stats_blue = aggregate_algo(match_dir, blue_algo)

    game = {
        "gameIndex": to_int(meta.get("matchIndex")),
        "side": side,
        "result": result_from_a_perspective(final, side),
        "finalScore": {"a": score_a, "b": score_b},
        "moves": final["moves"],
        "source": final["source"],
        "logs": str(match_dir.relative_to(root)),
        "stats": {
            "A": stats_red if side == "A_red" else stats_blue,
            "B": stats_blue if side == "A_red" else stats_red,
        },
    }
    return pair_a, pair_b, meta["map"], game


def weighted_tt_summary(rows, suffix=""):
    probes = sum(to_int(row.get(f"_ttProbes{suffix}")) for row in rows)
    if not probes:
        return {}
    hits = sum(to_int(row.get(f"_ttHits{suffix}")) for row in rows)
    cutoffs = sum(to_int(row.get(f"_ttCutoffs{suffix}")) for row in rows)
    move_hits = sum(to_int(row.get(f"_ttMoveHits{suffix}")) for row in rows)
    legal_moves = sum(to_int(row.get(f"_ttLegalMoves{suffix}")) for row in rows)

    hit_rate = pct(hits, probes)
    cutoff_rate = pct(cutoffs, probes)
    out = {
        f"ttHitRate{suffix}": hit_rate,
        f"ttCutoffRate{suffix}": cutoff_rate,
        f"ttCutoffPerHit{suffix}": pct(cutoffs, hits),
        f"ttMoveAvailability{suffix}": pct(move_hits, probes),
        f"ttLegalMoveAvailability{suffix}": pct(legal_moves, probes),
    }
    if suffix == "Core":
        out["meanHitRateCore"] = hit_rate
        out["meanCutoffRateCore"] = cutoff_rate
    elif suffix == "":
        out["meanHitRate"] = hit_rate
        out["meanCutoffRate"] = cutoff_rate
    return out


def weighted_order_summary(rows, suffix=""):
    tried_moves = sum(to_int(row.get(f"_orderTriedMoves{suffix}")) for row in rows)
    cutoffs = sum(to_int(row.get(f"_orderCutoffs{suffix}")) for row in rows)
    first_cutoffs = sum(to_int(row.get(f"_orderFirstCutoffs{suffix}")) for row in rows)
    cut_index_sum = sum(to_int(row.get(f"_orderCutIndexSum{suffix}")) for row in rows)
    root_iterations = sum(to_int(row.get(f"_rootIterations{suffix}")) for row in rows)
    root_first_best = sum(to_int(row.get(f"_rootFirstBest{suffix}")) for row in rows)
    root_best_rank_sum = sum(to_int(row.get(f"_rootBestRankSum{suffix}")) for row in rows)
    root_bound_updates = sum(to_int(row.get(f"_rootBoundUpdates{suffix}")) for row in rows)
    root_move_count_sum = sum(to_int(row.get(f"_rootMoveCountSum{suffix}")) for row in rows)
    if not tried_moves and not root_iterations:
        return {}

    out = {
        f"orderTriedMoves{suffix}": tried_moves,
        f"orderCutoffs{suffix}": cutoffs,
        f"orderCutoffRate{suffix}": pct(cutoffs, tried_moves),
        f"orderFirstCutoffRate{suffix}": pct(first_cutoffs, cutoffs),
        f"orderAvgCutIndex{suffix}": (cut_index_sum / cutoffs) if cutoffs else 0,
        f"rootFirstBestRate{suffix}": pct(root_first_best, root_iterations),
        f"rootAvgBestRank{suffix}": (root_best_rank_sum / root_iterations) if root_iterations else 0,
        f"rootBoundUpdateRate{suffix}": (root_bound_updates / root_iterations) if root_iterations else 0,
        f"rootAvgMoveCount{suffix}": (root_move_count_sum / root_iterations) if root_iterations else 0,
    }

    impact_total = 0
    for _, key in ORDER_SOURCES:
        title = key[0].upper() + key[1:]
        impact_total += sum(to_int(row.get(f"_order{title}CutDepth{suffix}")) for row in rows)

    for _, key in ORDER_SOURCES:
        title = key[0].upper() + key[1:]
        tried = sum(to_int(row.get(f"_order{title}Tried{suffix}")) for row in rows)
        source_cutoffs = sum(to_int(row.get(f"_order{title}Cutoffs{suffix}")) for row in rows)
        cut_depth = sum(to_int(row.get(f"_order{title}CutDepth{suffix}")) for row in rows)
        out.update({
            f"order{title}Tried{suffix}": tried,
            f"order{title}Cutoffs{suffix}": source_cutoffs,
            f"order{title}CutoffRate{suffix}": pct(source_cutoffs, tried),
            f"order{title}AvgCutDepth{suffix}": (cut_depth / source_cutoffs) if source_cutoffs else 0,
            f"order{title}ImpactShare{suffix}": pct(cut_depth, impact_total),
        })
    return out


def merge_numeric_stats(games, slot):
    stats_list = [g.get("stats", {}).get(slot, {}) for g in games]
    out = {}
    keys = set()
    for stats in stats_list:
        keys.update(
            k for k, v in stats.items()
            if isinstance(v, (int, float)) and not k.startswith("_")
        )
    for key in sorted(keys):
        vals = [s.get(key) for s in stats_list if isinstance(s.get(key), (int, float))]
        out[key] = compact_number(mean(vals))
    out.update(weighted_tt_summary(stats_list, "Core"))
    out.update(weighted_order_summary(stats_list, "Core"))

    phase_summary = {}
    for phase in DISPLAY_PHASES:
        phase_rows = [
            s.get("phases", {}).get(phase, {})
            for s in stats_list
            if isinstance(s.get("phases", {}).get(phase), dict)
        ]
        phase_keys = set()
        for row in phase_rows:
            phase_keys.update(
                k for k, v in row.items()
                if isinstance(v, (int, float)) and not k.startswith("_")
            )
        if phase_keys:
            phase_summary[phase] = {
                key: compact_number(mean([
                    row.get(key) for row in phase_rows
                    if isinstance(row.get(key), (int, float))
                ]))
                for key in sorted(phase_keys)
            }
            phase_summary[phase].update(weighted_tt_summary(phase_rows))
            phase_summary[phase].update(weighted_order_summary(phase_rows))
    if phase_summary:
        out["phases"] = phase_summary
    return out


def summarize_games(games):
    by_color = {
        "A_red": {"wins": 0, "draws": 0, "losses": 0},
        "A_blue": {"wins": 0, "draws": 0, "losses": 0},
    }
    wins = draws = losses = 0
    for game in games:
        result = game.get("result")
        side = game.get("side")
        bucket = by_color.setdefault(side, {"wins": 0, "draws": 0, "losses": 0})
        if result == "A_win":
            wins += 1
            bucket["wins"] += 1
        elif result == "B_win":
            losses += 1
            bucket["losses"] += 1
        elif result == "draw":
            draws += 1
            bucket["draws"] += 1

    return {
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "byColor": by_color,
        "avg": {
            "A": merge_numeric_stats(games, "A"),
            "B": merge_numeric_stats(games, "B"),
        },
    }


def build_results(root: Path, only_run: Path | None = None):
    run_root = root / "logs" / "tournament" / "runs"
    meta_paths = sorted(run_root.glob("*/match_*/meta.json"))
    if only_run is not None:
        only_run = only_run.resolve()
        meta_paths = [p for p in meta_paths if only_run in p.resolve().parents]

    seen_logs = set()
    results = {}
    skipped = 0
    for meta_path in meta_paths:
        match_dir = meta_path.parent
        built = build_game(root, match_dir)
        if not built:
            skipped += 1
            continue

        pair_a, pair_b, map_name, game = built
        log_key = game["logs"]
        if log_key in seen_logs:
            continue
        seen_logs.add(log_key)

        pair_key = f"{pair_a}__vs__{pair_b}"
        map_bucket = results.setdefault(pair_key, {"maps": {}})["maps"].setdefault(
            map_name, {"games": []}
        )
        map_bucket["games"].append(game)

    for pair in results.values():
        for map_bucket in pair["maps"].values():
            map_bucket["games"].sort(key=lambda g: (g.get("logs", ""), g.get("gameIndex", 0)))
            map_bucket["summary"] = summarize_games(map_bucket["games"])

    return results, len(seen_logs), skipped


def js_object(obj):
    return json.dumps(obj, indent=4, ensure_ascii=False)


def replace_const_object(text: str, const_name: str, replacement: str):
    needle = f"const {const_name} = "
    start = text.find(needle)
    if start < 0:
        return text.rstrip() + "\n\n" + replacement + "\n"

    brace_start = text.find("{", start)
    if brace_start < 0:
        raise ValueError(f"Cannot find opening brace for {const_name}")

    depth = 0
    in_str = None
    escaped = False
    end = None
    for i in range(brace_start, len(text)):
        ch = text[i]
        if in_str:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == in_str:
                in_str = None
            continue
        if ch in ("'", '"', "`"):
            in_str = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                j = i + 1
                while j < len(text) and text[j].isspace():
                    j += 1
                if j < len(text) and text[j] == ";":
                    j += 1
                end = j
                break

    if end is None:
        raise ValueError(f"Cannot find end of {const_name}")
    return text[:start] + replacement + text[end:]


def strip_private_keys(value):
    if isinstance(value, dict):
        return {
            key: strip_private_keys(child)
            for key, child in value.items()
            if not key.startswith("_")
        }
    if isinstance(value, list):
        return [strip_private_keys(child) for child in value]
    return value


def ensure_maps_const(text: str):
    if "const MAPS" in text:
        return text
    marker = "/* ============================================================================\n * 3) MATCH_RESULTS"
    maps_block = '''/* ============================================================================
 * 3) CARTES JOUÉES
 * ============================================================================
 * L'ordre détermine l'ordre des boutons dans le toggle.
 * ========================================================================== */

const MAPS = [
    { id: "standard", label: "Map standard", icon: "▦" },
    { id: "X",        label: "Map X",        icon: "✕" }
];


'''
    idx = text.find(marker)
    if idx < 0:
        return text + "\n\n" + maps_block
    text = text[:idx] + maps_block + text[idx:]
    return text.replace(" * 3) MATCH_RESULTS", " * 4) MATCH_RESULTS", 1)


def write_outputs(root: Path, results):
    logs_dir = root / "logs"
    public_results = strip_private_keys(results)
    generated_path = logs_dir / "data.generated.js"
    generated_path.write_text(
        "/* Auto-generated by scripts/aggregate.py. Do not edit by hand. */\n"
        "const GENERATED_MATCH_RESULTS = "
        + js_object(public_results)
        + ";\n\n"
        + 'if (typeof MATCH_RESULTS !== "undefined") {\n'
        + "    Object.assign(MATCH_RESULTS, GENERATED_MATCH_RESULTS);\n"
        + "}\n",
    )

    data_path = logs_dir / "data.js"
    text = read_text(data_path)
    text = ensure_maps_const(text)
    replacement = "const MATCH_RESULTS = " + js_object(public_results) + ";\n"
    text = replace_const_object(text, "MATCH_RESULTS", replacement)
    data_path.write_text(text.rstrip() + "\n")
    return generated_path, data_path


def main():
    parser = argparse.ArgumentParser(description="Aggregate BlobWar tournament logs into logs/data.js")
    parser.add_argument("--root", default=".", help="Repository root")
    parser.add_argument("--run", default=None, help="Optional run directory to aggregate")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    only_run = Path(args.run).resolve() if args.run else None
    results, imported, skipped = build_results(root, only_run)
    if imported == 0 and not results and only_run is None:
        print("[aggregate] imported games: 0")
        print(f"[aggregate] skipped/incomplete: {skipped}")
        print("[aggregate] no completed tournament logs found; leaving data files unchanged")
        return

    generated_path, data_path = write_outputs(root, results)

    print(f"[aggregate] imported games: {imported}")
    print(f"[aggregate] skipped/incomplete: {skipped}")
    print(f"[aggregate] generated: {generated_path}")
    print(f"[aggregate] updated: {data_path}")


if __name__ == "__main__":
    main()
