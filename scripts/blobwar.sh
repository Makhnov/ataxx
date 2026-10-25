#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat >&2 <<'USAGE'
Usage:
  ./scripts/blobwar.sh EXE1 EXE2 map nbDePartiesAvecUneCouleur

Example:
  ./scripts/blobwar.sh NMP_TT_SF_v1 NMP_TT_SF_v2 standard 10

Environment:
  BLOBWAR_PORT=2001              Server port to use.
  BLOBWAR_MATCH_TIMEOUT=300      Max seconds per game before killing it.
  BLOBWAR_CONNECT_DELAY=0.30     Delay between RED and BLUE launches.
  BLOBWAR_REPETITION_COUNT=3     Same full position count that forces a draw.
  BLOBWAR_REPETITION_MIN_MOVES=12 Ignore repetitions before this move count.
  BLOBWAR_PROGRESS=auto          Show one live terminal status line: auto|1|0.
  BLOBWAR_PROGRESS_INTERVAL=1    Seconds between live status refreshes.
USAGE
}

die() {
    echo "[blobwar] ERROR: $*" >&2
    exit 1
}

if [[ $# -ne 4 ]]; then
    usage
    exit 2
fi

EXE_A="$1"
EXE_B="$2"
MAP_NAME="$3"
GAMES_PER_SIDE="$4"

[[ "$GAMES_PER_SIDE" =~ ^[0-9]+$ ]] || die "nbDePartiesAvecUneCouleur must be an integer"
(( GAMES_PER_SIDE > 0 )) || die "nbDePartiesAvecUneCouleur must be > 0"
TOTAL_MATCHES=$((GAMES_PER_SIDE * 2))

if [[ -n "${BLOBWAR_ROOT_DIR:-}" ]]; then
    ROOT_DIR="$BLOBWAR_ROOT_DIR"
else
    ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
MM0_DIR="$ROOT_DIR/MM0"
SERVER_DIR="$ROOT_DIR/server"
SERVER_EXE="$SERVER_DIR/blobwar_server"
LOG_ROOT="$ROOT_DIR/logs/tournament"
RUNS_DIR="$LOG_ROOT/runs"
RAW_DIR="$LOG_ROOT/raw"
PAIR_DIR="$LOG_ROOT/pairs"

PORT="${BLOBWAR_PORT:-2001}"
MATCH_TIMEOUT="${BLOBWAR_MATCH_TIMEOUT:-300}"
CONNECT_DELAY="${BLOBWAR_CONNECT_DELAY:-0.30}"
REPETITION_COUNT="${BLOBWAR_REPETITION_COUNT:-3}"
REPETITION_MIN_MOVES="${BLOBWAR_REPETITION_MIN_MOVES:-12}"
PROGRESS="${BLOBWAR_PROGRESS:-auto}"
PROGRESS_INTERVAL="${BLOBWAR_PROGRESS_INTERVAL:-1}"

[[ -x "$SERVER_EXE" ]] || die "server executable not found: $SERVER_EXE"

resolve_exe() {
    local name="$1"
    if [[ "$name" == */* ]]; then
        [[ -x "$ROOT_DIR/$name" ]] && { printf '%s\n' "$ROOT_DIR/$name"; return; }
        [[ -x "$name" ]] && { printf '%s\n' "$name"; return; }
    else
        [[ -x "$MM0_DIR/$name" ]] && { printf '%s\n' "$MM0_DIR/$name"; return; }
        [[ -x "$ROOT_DIR/$name" ]] && { printf '%s\n' "$ROOT_DIR/$name"; return; }
    fi
    die "executable not found or not executable: $name"
}

RUN_ID="${BLOBWAR_RUN_ID:-$(date +%Y%m%d_%H%M%S)_${EXE_A}__vs__${EXE_B}_${MAP_NAME}_${GAMES_PER_SIDE}x2}"
RUN_DIR="$RUNS_DIR/$RUN_ID"
mkdir -p "$RUN_DIR" "$RAW_DIR" "$PAIR_DIR"

if [[ "${BLOBWAR_RUNNER_SNAPSHOT:-0}" != "1" ]]; then
    SNAPSHOT="$RUN_DIR/blobwar.runner.sh"
    cp "${BASH_SOURCE[0]}" "$SNAPSHOT"
    chmod +x "$SNAPSHOT"
    export BLOBWAR_RUNNER_SNAPSHOT=1
    export BLOBWAR_RUN_ID="$RUN_ID"
    export BLOBWAR_ROOT_DIR="$ROOT_DIR"
    exec "$SNAPSHOT" "$@"
fi

EXE_A_PATH="$(resolve_exe "$EXE_A")"
EXE_B_PATH="$(resolve_exe "$EXE_B")"

SERVER_PID=""
RED_PID=""
BLUE_PID=""
SCORE_A=0
SCORE_B=0
SCORE_DRAWS=0
SCORE_UNKNOWN=0

cleanup_processes() {
    local p
    for p in "$RED_PID" "$BLUE_PID" "$SERVER_PID"; do
        if [[ -n "${p:-}" ]] && kill -0 "$p" 2>/dev/null; then
            kill "$p" 2>/dev/null || true
        fi
    done
    sleep 0.2
    for p in "$RED_PID" "$BLUE_PID" "$SERVER_PID"; do
        if [[ -n "${p:-}" ]] && kill -0 "$p" 2>/dev/null; then
            kill -9 "$p" 2>/dev/null || true
        fi
    done
}

trap cleanup_processes EXIT INT TERM

progress_enabled() {
    if [[ "$PROGRESS" == "1" ]]; then
        return 0
    fi
    if [[ "$PROGRESS" == "0" ]]; then
        return 1
    fi
    [[ -t 1 ]]
}

format_duration() {
    local seconds="$1"
    printf '%02d:%02d' $((seconds / 60)) $((seconds % 60))
}

terminal_cols() {
    local cols
    cols="$(tput cols 2>/dev/null || printf '80')"
    [[ "$cols" =~ ^[0-9]+$ ]] || cols=80
    (( cols > 20 )) || cols=80
    printf '%s' "$cols"
}

clear_progress_line() {
    progress_enabled || return 0
    local cols
    cols="$(terminal_cols)"
    printf '\r%*s\r' $((cols - 1)) ""
}

print_match_progress() {
    progress_enabled || return 0

    local match_dir="$1"
    local match_index="$2"
    local red_algo="$3"
    local blue_algo="$4"
    local start_ts="$5"
    local deadline="$6"
    local log="$match_dir/server.log"

    local now elapsed remaining elapsed_fmt
    now="$(date +%s)"
    elapsed=$((now - start_ts))
    remaining=$((deadline - now))
    (( remaining < 0 )) && remaining=0
    elapsed_fmt="$(format_duration "$elapsed")"

    local moves=0
    if [[ -f "$log" ]]; then
        moves="$(grep -c "moved from" "$log" 2>/dev/null || true)"
    fi

    local last_line=""
    local turn="-"
    if [[ -f "$log" ]]; then
        last_line="$(grep " :turn: " "$log" 2>/dev/null | tail -n 1 || true)"
    fi
    if [[ "$last_line" =~ turn:[[:space:]]*([0-9]+) ]]; then
        turn="${BASH_REMATCH[1]}"
    elif (( moves > 0 )); then
        turn="$moves"
    fi

    local side_match
    side_match=$(( ((match_index - 1) % GAMES_PER_SIDE) + 1 ))
    local line
    line="$(printf '[blobwar] %02d/%02d score=%d-%d-%d turn=%s t=%s | %s(R) vs %s(B)' \
        "$side_match" "$GAMES_PER_SIDE" "$SCORE_A" "$SCORE_B" "$SCORE_DRAWS" \
        "$turn" "$elapsed_fmt" "$red_algo" "$blue_algo")"

    local cols max
    cols="$(terminal_cols)"
    max=$((cols - 1))
    if (( ${#line} > max )); then
        line="${line:0:max}"
    fi

    printf '\r%*s\r%s' $((cols - 1)) "" "$line"
}

record_match_score() {
    local side="$1"
    local match_dir="$2"
    local forced="$match_dir/forced_result.json"
    if [[ -f "$forced" ]]; then
        SCORE_DRAWS=$((SCORE_DRAWS + 1))
        return 0
    fi

    local server_log="$match_dir/server.log"
    local red_score=""
    local blue_score=""
    if [[ -f "$server_log" ]]; then
        red_score="$(sed -n 's/^player 0:[[:space:]]*//p' "$server_log" | tail -n 1)"
        blue_score="$(sed -n 's/^player 1:[[:space:]]*//p' "$server_log" | tail -n 1)"
    fi

    if ! [[ "$red_score" =~ ^[0-9]+$ && "$blue_score" =~ ^[0-9]+$ ]]; then
        SCORE_UNKNOWN=$((SCORE_UNKNOWN + 1))
        return 0
    fi

    if (( red_score == blue_score )); then
        SCORE_DRAWS=$((SCORE_DRAWS + 1))
    elif [[ "$side" == "A_red" ]]; then
        if (( red_score > blue_score )); then
            SCORE_A=$((SCORE_A + 1))
        else
            SCORE_B=$((SCORE_B + 1))
        fi
    else
        if (( blue_score > red_score )); then
            SCORE_A=$((SCORE_A + 1))
        else
            SCORE_B=$((SCORE_B + 1))
        fi
    fi
}

detect_draw_by_repetition() {
    local match_dir="$1"
    local forced_json="$match_dir/forced_result.json"
    [[ -f "$forced_json" ]] && return 0

    python3 - "$ROOT_DIR" "$MAP_NAME" "$match_dir/server.log" "$forced_json" \
        "$REPETITION_COUNT" "$REPETITION_MIN_MOVES" <<'PY'
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
map_name = sys.argv[2]
server_log = Path(sys.argv[3])
forced_json = Path(sys.argv[4])
repeat_limit = int(sys.argv[5])
min_moves = int(sys.argv[6])

if not server_log.exists():
    sys.exit(1)

board_path = root / "data" / "boards" / map_name
try:
    lines = board_path.read_text().splitlines()
except FileNotFoundError:
    sys.exit(1)

holes = set()
for y, line in enumerate(lines[:8]):
    for x, ch in enumerate(line[:8]):
        if ch == "x":
            holes.add((x, y))

red = {(0, 0), (7, 0)}
blue = {(0, 7), (7, 7)}
red -= holes
blue -= holes

def key(next_player):
    return (
        tuple(sorted(red)),
        tuple(sorted(blue)),
        next_player,
    )

def score():
    return len(red), len(blue)

def apply_move(player, fx, fy, tx, ty):
    mine = red if player == 0 else blue
    opp = blue if player == 0 else red
    src = (fx, fy)
    dst = (tx, ty)
    if src not in mine or dst in red or dst in blue or dst in holes:
        return False

    if max(abs(tx - fx), abs(ty - fy)) == 2:
        mine.remove(src)
    mine.add(dst)

    flips = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            sq = (tx + dx, ty + dy)
            if sq in opp:
                flips.append(sq)
    for sq in flips:
        opp.remove(sq)
        mine.add(sq)
    return True

seen = {key(1): 1}  # The legacy server starts with BLUE/player 1.
pending = None
moves = 0

sent_re = re.compile(
    r'player:\s*([01])\s+sent\s+:"/do_move\s+\((\d+),(\d+)\)\s+to\s+\((\d+),(\d+)\)"'
)
accepted_re = re.compile(
    r"move from \((\d+),(\d+)\) to \((\d+),(\d+)\) accepted"
)

for line in server_log.read_text(errors="replace").splitlines():
    sent = sent_re.search(line)
    if sent:
        pending = tuple(map(int, sent.groups()))
        continue

    accepted = accepted_re.search(line)
    if not accepted or pending is None:
        continue

    player, fx, fy, tx, ty = pending
    accepted_move = tuple(map(int, accepted.groups()))
    if accepted_move != (fx, fy, tx, ty):
        pending = None
        continue

    pending = None
    if not apply_move(player, fx, fy, tx, ty):
        continue

    moves += 1
    next_player = 1 - player
    pos_key = key(next_player)
    seen[pos_key] = seen.get(pos_key, 0) + 1

    if moves >= min_moves and seen[pos_key] >= repeat_limit:
        r, b = score()
        forced_json.write_text(json.dumps({
            "forcedResult": "draw",
            "reason": "repetition",
            "repeatCount": seen[pos_key],
            "repeatLimit": repeat_limit,
            "moves": moves,
            "red": r,
            "blue": b,
            "nextPlayer": next_player,
            "map": map_name,
        }, indent=2) + "\n")
        sys.exit(0)

sys.exit(1)
PY
}

wait_for_exit_or_timeout() {
    local deadline="$1"
    local match_dir="$2"
    local match_index="$3"
    local red_algo="$4"
    local blue_algo="$5"
    local start_ts="$6"
    local now
    local last_repetition_check=0
    local last_progress_check=0
    while true; do
        local red_alive=0
        local blue_alive=0
        kill -0 "$RED_PID" 2>/dev/null && red_alive=1 || true
        kill -0 "$BLUE_PID" 2>/dev/null && blue_alive=1 || true
        (( red_alive == 0 && blue_alive == 0 )) && return 0

        now="$(date +%s)"
        if (( now >= deadline )); then
            return 124
        fi

        if (( now - last_progress_check >= PROGRESS_INTERVAL )); then
            last_progress_check="$now"
            print_match_progress "$match_dir" "$match_index" "$red_algo" "$blue_algo" "$start_ts" "$deadline"
        fi

        if (( now > last_repetition_check )); then
            last_repetition_check="$now"
            if detect_draw_by_repetition "$match_dir"; then
                return 125
            fi
        fi
        sleep 0.2
    done
}

write_meta() {
    local match_dir="$1"
    local match_index="$2"
    local side="$3"
    local red_algo="$4"
    local blue_algo="$5"
    cat > "$match_dir/meta.json" <<META
{
  "runId": "$RUN_ID",
  "matchIndex": $match_index,
  "pairA": "$EXE_A",
  "pairB": "$EXE_B",
  "side": "$side",
  "map": "$MAP_NAME",
  "redAlgo": "$red_algo",
  "blueAlgo": "$blue_algo",
  "port": $PORT
}
META
}

run_match() {
    local match_index="$1"
    local side="$2"
    local red_algo="$3"
    local red_exe="$4"
    local blue_algo="$5"
    local blue_exe="$6"

    local match_label
    match_label="$(printf 'match_%03d_%s_RED__%s_BLUE' "$match_index" "$red_algo" "$blue_algo")"
    local match_dir="$RUN_DIR/$match_label"
    mkdir -p "$match_dir"
    write_meta "$match_dir" "$match_index" "$side" "$red_algo" "$blue_algo"

    if ! progress_enabled; then
        echo "[blobwar] #$match_index RED=$red_algo BLUE=$blue_algo map=$MAP_NAME"
    fi

    SERVER_PID=""
    RED_PID=""
    BLUE_PID=""

    (
        cd "$SERVER_DIR"
        exec "$SERVER_EXE" "$PORT"
    ) >"$match_dir/server.log" 2>&1 &
    SERVER_PID=$!
    sleep 0.35

    (
        cd "$MM0_DIR"
        exec "$red_exe" \
            SERVER=localhost \
            PORT="$PORT" \
            MAP="$MAP_NAME" \
            TEAM="$red_algo" \
            ALGO="$red_algo" \
            ALGO_NAME="$red_algo" \
            LOG_DIR="$match_dir" \
            LOG_FILES=1
    ) >"$match_dir/red.log" 2>&1 &
    RED_PID=$!

    sleep "$CONNECT_DELAY"

    (
        cd "$MM0_DIR"
        exec "$blue_exe" \
            SERVER=localhost \
            PORT="$PORT" \
            TEAM="$blue_algo" \
            ALGO="$blue_algo" \
            ALGO_NAME="$blue_algo" \
            LOG_DIR="$match_dir" \
            LOG_FILES=1
    ) >"$match_dir/blue.log" 2>&1 &
    BLUE_PID=$!

    local deadline
    local start_ts
    start_ts="$(date +%s)"
    deadline=$(( start_ts + MATCH_TIMEOUT ))
    local status=0
    wait_for_exit_or_timeout "$deadline" "$match_dir" "$match_index" "$red_algo" "$blue_algo" "$start_ts" || status=$?
    clear_progress_line

    if (( status == 124 )); then
        echo "[blobwar] #$match_index timed out after ${MATCH_TIMEOUT}s" | tee "$match_dir/status.log"
        cleanup_processes
    elif (( status == 125 )); then
        echo "[blobwar] #$match_index draw by repetition" | tee "$match_dir/status.log"
        cleanup_processes
    else
        wait "$RED_PID" 2>/dev/null || true
        wait "$BLUE_PID" 2>/dev/null || true
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill "$SERVER_PID" 2>/dev/null || true
            wait "$SERVER_PID" 2>/dev/null || true
        fi
        echo "ok" > "$match_dir/status.log"
    fi

    record_match_score "$side" "$match_dir"
    echo "[blobwar] #$match_index done score=${SCORE_A}-${SCORE_B}-${SCORE_DRAWS}"

    SERVER_PID=""
    RED_PID=""
    BLUE_PID=""

    sleep 0.3
}

match_index=1
for ((i = 1; i <= GAMES_PER_SIDE; ++i)); do
    run_match "$match_index" "A_red" "$EXE_A" "$EXE_A_PATH" "$EXE_B" "$EXE_B_PATH"
    match_index=$((match_index + 1))
done

for ((i = 1; i <= GAMES_PER_SIDE; ++i)); do
    run_match "$match_index" "A_blue" "$EXE_B" "$EXE_B_PATH" "$EXE_A" "$EXE_A_PATH"
    match_index=$((match_index + 1))
done

trap - EXIT INT TERM
cleanup_processes

python3 "$ROOT_DIR/scripts/aggregate.py" --root "$ROOT_DIR"
echo "[blobwar] done: $RUN_DIR"
exit 0

python3 - "$ROOT_DIR" "$RUN_DIR" "$LOG_ROOT" "$EXE_A" "$EXE_B" "$MAP_NAME" <<'PY'
import csv
import json
import math
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
run_dir = Path(sys.argv[2])
log_root = Path(sys.argv[3])
algo_a = sys.argv[4]
algo_b = sys.argv[5]
map_name = sys.argv[6]

raw_dir = log_root / "raw"
pair_dir = log_root / "pairs"
raw_dir.mkdir(parents=True, exist_ok=True)
pair_dir.mkdir(parents=True, exist_ok=True)

CORE_PHASES = {"opening", "midgame", "endgame"}
ALL_PHASES = ["opening", "midgame", "endgame", "terminal"]

def read_text(path):
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""

def parse_key_values(line):
    out = {}
    for token in line.strip().split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out

def parse_final_from_logs(match_dir):
    forced_path = match_dir / "forced_result.json"
    if forced_path.exists():
        try:
            forced = json.loads(read_text(forced_path))
            if forced.get("forcedResult") == "draw":
                return {
                    "red": int(forced.get("red", 0)),
                    "blue": int(forced.get("blue", 0)),
                    "moves": int(forced.get("moves", 0)),
                    "outcome": "DRAW",
                    "source": forced.get("reason", "forced_draw"),
                    "forcedResult": "draw",
                    "repeatCount": forced.get("repeatCount"),
                }
        except (json.JSONDecodeError, TypeError, ValueError):
            pass

    candidates = []
    for name in ("red.log", "blue.log"):
        for line in read_text(match_dir / name).splitlines():
            if "[MM0GAME]" in line and "event=final" in line:
                candidates.append(parse_key_values(line))
    if candidates:
        final = candidates[-1]
        try:
            return {
                "red": int(final["red"]),
                "blue": int(final["blue"]),
                "moves": int(final.get("moves", 0)),
                "outcome": final.get("outcome", ""),
                "source": "client",
            }
        except (KeyError, ValueError):
            pass

    server_log = read_text(match_dir / "server.log")
    red = blue = None
    for line in server_log.splitlines():
        m0 = re.search(r"player 0:\s*(\d+)", line)
        m1 = re.search(r"player 1:\s*(\d+)", line)
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
    return {"red": None, "blue": None, "moves": 0, "outcome": "unknown", "source": "missing"}

def read_semicolon_csv(path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f, delimiter=";"))

def to_int(value, default=0):
    try:
        if value is None or value == "":
            return default
        return int(float(value))
    except ValueError:
        return default

def to_float(value, default=0.0):
    try:
        if value is None or value == "":
            return default
        return float(value)
    except ValueError:
        return default

def median(values):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return 0
    n = len(vals)
    mid = n // 2
    if n % 2:
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

def phase_stats(move_rows, phase_set):
    rows = [r for r in move_rows if r.get("OccupiedPhase") in phase_set]
    depths = [to_int(r.get("Depth")) for r in rows]
    nodes = [to_int(r.get("Nodes")) for r in rows]
    occupied_pcts = [to_float(r.get("OccupiedPct")) for r in rows]
    return {
        "turns": len(rows),
        "medianDepth": median(depths),
        "p75Depth": percentile(depths, 0.75),
        "maxDepth": max(depths) if depths else 0,
        "medianNodes": median(nodes),
        "totalNodes": sum(nodes),
        "medianOccupiedPct": median(occupied_pcts),
    }

def last_search_rows(search_rows):
    best = {}
    for row in search_rows:
        if row.get("Phase") != "my_turn":
            continue
        key = (row.get("TurnIndex"), row.get("OccupiedPhase"))
        depth = to_int(row.get("Depth"))
        if key not in best or depth >= to_int(best[key].get("Depth")):
            best[key] = row
    return list(best.values())

def aggregate_algo(match_dir, algo):
    raw = match_dir / "raw"
    move_rows = read_semicolon_csv(raw / f"{algo}.moves.csv")
    search_rows = read_semicolon_csv(raw / f"{algo}.search.csv")
    move_rows = [r for r in move_rows if r.get("Phase") == "my_turn"]
    final_search = last_search_rows(search_rows)
    core_moves = [r for r in move_rows if r.get("OccupiedPhase") in CORE_PHASES]
    core_search = [r for r in final_search if r.get("OccupiedPhase") in CORE_PHASES]
    all_core_search = [r for r in search_rows if r.get("Phase") == "my_turn" and r.get("OccupiedPhase") in CORE_PHASES]

    phases = {phase: phase_stats(move_rows, {phase}) for phase in ALL_PHASES}
    core = phase_stats(move_rows, CORE_PHASES)
    core["medianNps"] = median([to_int(r.get("NPS")) for r in core_search])

    shortcut_values = [r.get("Shortcut", "none") for r in move_rows]
    shortcut_hits = sum(1 for v in shortcut_values if v in {"hit", "carry_hit", "useful_hit"})
    shortcut_misses = sum(1 for v in shortcut_values if v in {"miss", "carry_miss"})
    shortcut_attempts = shortcut_hits + shortcut_misses
    useful_hits = sum(1 for v in shortcut_values if v == "useful_hit")

    return {
        "searchCore": core,
        "phases": phases,
        "tt": {
            "meanHitRate": mean([to_float(r.get("TTHitRate")) for r in core_search]),
            "meanCutoffRate": mean([to_float(r.get("TTCutoffRate")) for r in core_search]),
            "meanOrderRate": mean([to_float(r.get("TTOrderRate")) for r in core_search]),
            "meanFillRate": mean([to_float(r.get("TTFillRate")) for r in core_search]),
            "totalOverwrites": sum(to_int(r.get("TTOverwrites")) for r in all_core_search),
            "totalRejected": sum(to_int(r.get("TTRejected")) for r in all_core_search),
        },
        "shortcut": {
            "attempts": shortcut_attempts,
            "hits": shortcut_hits,
            "misses": shortcut_misses,
            "hitRate": (100.0 * shortcut_hits / shortcut_attempts) if shortcut_attempts else 0,
            "usefulHits": useful_hits,
            "usefulHitRate": (100.0 * useful_hits / shortcut_hits) if shortcut_hits else 0,
            "medianDepthOnHit": median([to_int(r.get("Depth")) for r in move_rows if r.get("Shortcut") in {"hit", "carry_hit", "useful_hit"}]),
            "medianDepthOnMiss": median([to_int(r.get("Depth")) for r in move_rows if r.get("Shortcut") in {"miss", "carry_miss"}]),
        },
    }

def append_unique_csv(src, dst):
    if not src.exists():
        return
    with src.open() as f:
        lines = f.readlines()
    if not lines:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or dst.stat().st_size == 0:
        dst.write_text("".join(lines))
    else:
        with dst.open("a") as out:
            out.writelines(lines[1:])

match_entries = []
run_rows = []

for meta_path in sorted(run_dir.glob("match_*/meta.json")):
    match_dir = meta_path.parent
    meta = json.loads(read_text(meta_path))
    final = parse_final_from_logs(match_dir)
    red_algo = meta["redAlgo"]
    blue_algo = meta["blueAlgo"]
    side = meta["side"]

    if final["red"] is None or final["blue"] is None:
        result = "unknown"
        score_a = score_b = None
    else:
        red_wins = final["red"] > final["blue"]
        blue_wins = final["blue"] > final["red"]
        if side == "A_red":
            score_a, score_b = final["red"], final["blue"]
        else:
            score_a, score_b = final["blue"], final["red"]

        if final.get("forcedResult") == "draw":
            result = "draw"
        elif side == "A_red":
            result = "A_win" if red_wins else "B_win" if blue_wins else "draw"
        else:
            result = "A_win" if blue_wins else "B_win" if red_wins else "draw"

    for algo in {red_algo, blue_algo}:
        for suffix in ("search", "moves", "root_ranking", "games"):
            append_unique_csv(match_dir / "raw" / f"{algo}.{suffix}.csv",
                              raw_dir / f"{algo}.{suffix}.csv")

    stats_red = aggregate_algo(match_dir, red_algo)
    stats_blue = aggregate_algo(match_dir, blue_algo)

    entry = {
        "side": side,
        "result": result,
        "finalScore": {"a": score_a, "b": score_b},
        "moves": final["moves"],
        "source": final["source"],
        "map": meta["map"],
        "logs": str(match_dir.relative_to(root)),
        "stats": {
            "A": stats_red if side == "A_red" else stats_blue,
            "B": stats_blue if side == "A_red" else stats_red,
        },
    }
    match_entries.append(entry)

    run_rows.append({
        "RunId": meta["runId"],
        "MatchIndex": meta["matchIndex"],
        "Pair": f"{algo_a}__vs__{algo_b}",
        "Map": meta["map"],
        "Side": side,
        "RedAlgo": red_algo,
        "BlueAlgo": blue_algo,
        "RedScore": final["red"],
        "BlueScore": final["blue"],
        "Result": result,
        "Moves": final["moves"],
        "Outcome": final["outcome"],
        "Source": final["source"],
        "LogDir": str(match_dir.relative_to(root)),
    })

pair_key = f"{algo_a}__vs__{algo_b}"
pair_json = pair_dir / f"{pair_key}.summary.json"

existing_pair = {}
if pair_json.exists():
    try:
        existing_pair = json.loads(pair_json.read_text())
    except json.JSONDecodeError:
        existing_pair = {}

existing_games = existing_pair.get(pair_key, {}).get("games", [])
existing_maps = existing_pair.get(pair_key, {}).get("maps", {})
existing_map_games = existing_maps.get(map_name, {}).get("games", [])

combined_map_games = existing_map_games + match_entries
existing_maps[map_name] = {
    "map": map_name,
    "games": combined_map_games,
}

combined_games = []
for map_bucket in existing_maps.values():
    combined_games.extend(map_bucket.get("games", []))

generated = {
    pair_key: {
        "games": combined_games,
        "maps": existing_maps,
    }
}
pair_json.write_text(json.dumps(generated, indent=2, ensure_ascii=False) + "\n")

all_generated = {}
for summary_path in sorted(pair_dir.glob("*.summary.json")):
    try:
        summary = json.loads(summary_path.read_text())
    except json.JSONDecodeError:
        continue
    for key, value in summary.items():
        if key not in all_generated:
            all_generated[key] = value
            continue
        merged_maps = all_generated[key].setdefault("maps", {})
        for map_id, map_value in value.get("maps", {}).items():
            merged_maps[map_id] = map_value
        merged_games = []
        for map_bucket in merged_maps.values():
            merged_games.extend(map_bucket.get("games", []))
        all_generated[key]["games"] = merged_games

data_generated = root / "logs" / "data.generated.js"
data_generated.write_text(
    "/* Auto-generated by scripts/blobwar.sh. Do not edit by hand. */\n"
    "const GENERATED_MATCH_RESULTS = "
    + json.dumps(all_generated, indent=4, ensure_ascii=False)
    + ";\n\n"
      "if (typeof MATCH_RESULTS !== \"undefined\") {\n"
      "    Object.assign(MATCH_RESULTS, GENERATED_MATCH_RESULTS);\n"
      "}\n"
)

data_js = root / "logs" / "data.js"
if data_js.exists():
    replacement = "const MATCH_RESULTS = " + json.dumps(all_generated, indent=4, ensure_ascii=False) + ";\n"
    text = data_js.read_text()
    new_text, count = re.subn(
        r"const\s+MATCH_RESULTS\s*=\s*\{.*?\};\s*$",
        replacement,
        text,
        count=1,
        flags=re.S,
    )
    if count:
        data_js.write_text(new_text)

run_csv = run_dir / "results.csv"
global_csv = log_root / "results.csv"
fieldnames = [
    "RunId", "MatchIndex", "Pair", "Map", "Side", "RedAlgo", "BlueAlgo",
    "RedScore", "BlueScore", "Result", "Moves", "Outcome", "Source", "LogDir"
]

for path, append in ((run_csv, False), (global_csv, True)):
    write_header = (not append) or (not path.exists()) or path.stat().st_size == 0
    with path.open("a" if append else "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter=";")
        if write_header:
            writer.writeheader()
        writer.writerows(run_rows)

wins_a = sum(1 for e in match_entries if e["result"] == "A_win")
wins_b = sum(1 for e in match_entries if e["result"] == "B_win")
draws = sum(1 for e in match_entries if e["result"] == "draw")
unknown = sum(1 for e in match_entries if e["result"] == "unknown")

print(f"[blobwar] aggregated pair: {pair_key}")
print(f"[blobwar] score: {algo_a}={wins_a} {algo_b}={wins_b} draw={draws} unknown={unknown}")
print(f"[blobwar] run csv: {run_csv}")
print(f"[blobwar] global csv: {global_csv}")
print(f"[blobwar] generated js: {data_generated}")
print(f"[blobwar] updated data.js: {data_js}")
print(f"[blobwar] pair json: {pair_json}")
PY

echo "[blobwar] done: $RUN_DIR"
