#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat >&2 <<'USAGE'
Usage:
  ./scripts/blobwar_remote_server.sh ALGO_A ALGO_B map nbDePartiesAvecUneCouleur

Example:
  BLOBWAR_PORT=2002 ./scripts/blobwar_remote_server.sh NMP_TT_SF_v1 NMP_TT_SF_v4 standard 10

This script runs only the legacy server and coordinates remote players.
It does not start any AI process. Remote players should run:

  ./scripts/blobwar_remote_player.sh SERVER=<server-ip> PORT=<port> \
      ALGO=<ALGO_A> OPP=<ALGO_B> PAIR=A MAP=<map> GAMES_PER_SIDE=<n> RUN_ID=<run-id>

  ./scripts/blobwar_remote_player.sh SERVER=<server-ip> PORT=<port> \
      ALGO=<ALGO_B> OPP=<ALGO_A> PAIR=B MAP=<map> GAMES_PER_SIDE=<n> RUN_ID=<run-id>

Environment:
  BLOBWAR_PORT=2001
  BLOBWAR_SERVER_HOST=<printed in client commands; auto-detected if empty>
  BLOBWAR_MATCH_TIMEOUT=7600
  BLOBWAR_REPETITION_COUNT=3
  BLOBWAR_REPETITION_MIN_MOVES=12
  BLOBWAR_PROGRESS=auto
  BLOBWAR_PROGRESS_INTERVAL=1
USAGE
}

die() {
    echo "[remote-server] ERROR: $*" >&2
    exit 1
}

if [[ $# -eq 1 && ( "$1" == "-h" || "$1" == "--help" || "$1" == "help" ) ]]; then
    usage
    exit 0
fi

if [[ $# -ne 4 ]]; then
    usage
    exit 2
fi

ALGO_A="$1"
ALGO_B="$2"
MAP_NAME="$3"
GAMES_PER_SIDE="$4"

[[ "$GAMES_PER_SIDE" =~ ^[0-9]+$ ]] || die "nbDePartiesAvecUneCouleur must be an integer"
(( GAMES_PER_SIDE > 0 )) || die "nbDePartiesAvecUneCouleur must be > 0"
TOTAL_MATCHES=$((GAMES_PER_SIDE * 2))

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="$ROOT_DIR/server"
SERVER_EXE="$SERVER_DIR/blobwar_server"
LOG_ROOT="$ROOT_DIR/logs/tournament"
RUNS_DIR="$LOG_ROOT/runs"

PORT="${BLOBWAR_PORT:-2001}"
SERVER_HOST="${BLOBWAR_SERVER_HOST:-}"
MATCH_TIMEOUT="${BLOBWAR_MATCH_TIMEOUT:-7600}"
REPETITION_COUNT="${BLOBWAR_REPETITION_COUNT:-3}"
REPETITION_MIN_MOVES="${BLOBWAR_REPETITION_MIN_MOVES:-12}"
PROGRESS="${BLOBWAR_PROGRESS:-auto}"
PROGRESS_INTERVAL="${BLOBWAR_PROGRESS_INTERVAL:-1}"

[[ -x "$SERVER_EXE" ]] || die "server executable not found: $SERVER_EXE"

if [[ -z "$SERVER_HOST" ]]; then
    SERVER_HOST="$(hostname -I 2>/dev/null | awk '{print $1}')"
    [[ -n "$SERVER_HOST" ]] || SERVER_HOST="$(hostname)"
fi

RUN_ID="${BLOBWAR_RUN_ID:-$(date +%Y%m%d_%H%M%S)_${ALGO_A}__vs__${ALGO_B}_${MAP_NAME}_${GAMES_PER_SIDE}x2_remote}"
RUN_DIR="$RUNS_DIR/$RUN_ID"
mkdir -p "$RUN_DIR"

SERVER_PID=""
SCORE_A=0
SCORE_B=0
SCORE_DRAWS=0
SCORE_UNKNOWN=0

cleanup_server() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        sleep 0.2
    fi
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
}

trap cleanup_server EXIT INT TERM

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

match_label() {
    local match_index="$1"
    local red_algo="$2"
    local blue_algo="$3"
    printf 'match_%03d_%s_RED__%s_BLUE' "$match_index" "$red_algo" "$blue_algo"
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
  "pairA": "$ALGO_A",
  "pairB": "$ALGO_B",
  "side": "$side",
  "map": "$MAP_NAME",
  "redAlgo": "$red_algo",
  "blueAlgo": "$blue_algo",
  "port": $PORT,
  "remote": true
}
META
}

write_plan() {
    cat > "$RUN_DIR/remote_plan.env" <<PLAN
RUN_ID=$RUN_ID
SERVER=$SERVER_HOST
PORT=$PORT
MAP=$MAP_NAME
GAMES_PER_SIDE=$GAMES_PER_SIDE
ALGO_A=$ALGO_A
ALGO_B=$ALGO_B
PLAN

    cat > "$RUN_DIR/remote_client_commands.txt" <<CMDS
# Player A machine:
./scripts/blobwar_remote_player.sh SERVER=$SERVER_HOST PORT=$PORT ALGO=$ALGO_A OPP=$ALGO_B PAIR=A MAP=$MAP_NAME GAMES_PER_SIDE=$GAMES_PER_SIDE RUN_ID=$RUN_ID

# Player B machine:
./scripts/blobwar_remote_player.sh SERVER=$SERVER_HOST PORT=$PORT ALGO=$ALGO_B OPP=$ALGO_A PAIR=B MAP=$MAP_NAME GAMES_PER_SIDE=$GAMES_PER_SIDE RUN_ID=$RUN_ID
CMDS
}

print_match_progress() {
    progress_enabled || return 0

    local match_dir="$1"
    local match_index="$2"
    local red_algo="$3"
    local blue_algo="$4"
    local start_ts="$5"
    local log="$match_dir/server.log"
    local now elapsed elapsed_fmt moves turn line cols max

    now="$(date +%s)"
    elapsed=$((now - start_ts))
    elapsed_fmt="$(format_duration "$elapsed")"

    moves=0
    if [[ -f "$log" ]]; then
        moves="$(grep -c "moved from" "$log" 2>/dev/null || true)"
    fi

    turn="-"
    if [[ -f "$log" ]]; then
        local last_line=""
        last_line="$(grep " :turn: " "$log" 2>/dev/null | tail -n 1 || true)"
        if [[ "$last_line" =~ turn:[[:space:]]*([0-9]+) ]]; then
            turn="${BASH_REMATCH[1]}"
        elif (( moves > 0 )); then
            turn="$moves"
        fi
    fi

    line="$(printf '[remote-server] %02d/%02d score=%d-%d-%d turn=%s t=%s | waiting clients | %s(R) vs %s(B)' \
        "$match_index" "$TOTAL_MATCHES" "$SCORE_A" "$SCORE_B" "$SCORE_DRAWS" \
        "$turn" "$elapsed_fmt" "$red_algo" "$blue_algo")"

    cols="$(terminal_cols)"
    max=$((cols - 1))
    if (( ${#line} > max )); then
        line="${line:0:max}"
    fi
    printf '\r%*s\r%s' $((cols - 1)) "" "$line"
}

server_final_seen() {
    local log="$1"
    [[ -f "$log" ]] || return 1
    grep -q '^player 0:[[:space:]]*[0-9]\+' "$log" &&
        grep -q '^player 1:[[:space:]]*[0-9]\+' "$log"
}

validate_player_order() {
    local log="$1"
    local red_algo="$2"
    local blue_algo="$3"
    [[ -f "$log" ]] || return 0

    local p0=""
    local p1=""
    p0="$(sed -n 's/^player 0 : //p' "$log" | tail -n 1)"
    p1="$(sed -n 's/^player 1 : //p' "$log" | tail -n 1)"

    if [[ -z "$p0" || -z "$p1" ]]; then
        return 0
    fi
    [[ "$p0" == "$red_algo" && "$p1" == "$blue_algo" ]]
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

red = {(0, 0), (7, 0)} - holes
blue = {(0, 7), (7, 7)} - holes

def key(next_player):
    return (tuple(sorted(red)), tuple(sorted(blue)), next_player)

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

seen = {key(1): 1}
pending = None
moves = 0
sent_re = re.compile(
    r'player:\s*([01])\s+sent\s+:"/do_move\s+\((\d+),(\d+)\)\s+to\s+\((\d+),(\d+)\)"'
)
accepted_re = re.compile(r"move from \((\d+),(\d+)\) to \((\d+),(\d+)\) accepted")

for line in server_log.read_text(errors="replace").splitlines():
    sent = sent_re.search(line)
    if sent:
        pending = tuple(map(int, sent.groups()))
        continue
    accepted = accepted_re.search(line)
    if not accepted or pending is None:
        continue
    player, fx, fy, tx, ty = pending
    if tuple(map(int, accepted.groups())) != (fx, fy, tx, ty):
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

record_match_score() {
    local side="$1"
    local match_dir="$2"
    local forced="$match_dir/forced_result.json"
    if [[ -f "$forced" ]]; then
        SCORE_DRAWS=$((SCORE_DRAWS + 1))
        return 0
    fi

    local red_score=""
    local blue_score=""
    red_score="$(sed -n 's/^player 0:[[:space:]]*//p' "$match_dir/server.log" | tail -n 1)"
    blue_score="$(sed -n 's/^player 1:[[:space:]]*//p' "$match_dir/server.log" | tail -n 1)"

    if ! [[ "$red_score" =~ ^[0-9]+$ && "$blue_score" =~ ^[0-9]+$ ]]; then
        SCORE_UNKNOWN=$((SCORE_UNKNOWN + 1))
        return 0
    fi

    if (( red_score == blue_score )); then
        SCORE_DRAWS=$((SCORE_DRAWS + 1))
    elif [[ "$side" == "A_red" ]]; then
        if (( red_score > blue_score )); then SCORE_A=$((SCORE_A + 1)); else SCORE_B=$((SCORE_B + 1)); fi
    else
        if (( blue_score > red_score )); then SCORE_A=$((SCORE_A + 1)); else SCORE_B=$((SCORE_B + 1)); fi
    fi
}

run_match() {
    local match_index="$1"
    local side="$2"
    local red_algo="$3"
    local blue_algo="$4"
    local label
    label="$(match_label "$match_index" "$red_algo" "$blue_algo")"
    local match_dir="$RUN_DIR/$label"
    mkdir -p "$match_dir"
    write_meta "$match_dir" "$match_index" "$side" "$red_algo" "$blue_algo"

    echo "[remote-server] #$match_index/$TOTAL_MATCHES RED=$red_algo BLUE=$blue_algo map=$MAP_NAME port=$PORT"
    echo "[remote-server] waiting for remote clients; server logs: $match_dir"

    SERVER_PID=""
    (
        cd "$SERVER_DIR"
        exec "$SERVER_EXE" "$PORT"
    ) >"$match_dir/server.log" 2>&1 &
    SERVER_PID=$!

    local start_ts deadline now status last_progress=0 last_repetition=0 order_checked=0
    start_ts="$(date +%s)"
    deadline=$((start_ts + MATCH_TIMEOUT))
    status=0

    while true; do
        now="$(date +%s)"

        if (( now - last_progress >= PROGRESS_INTERVAL )); then
            last_progress="$now"
            print_match_progress "$match_dir" "$match_index" "$red_algo" "$blue_algo" "$start_ts"
        fi

        if (( order_checked == 0 )) && grep -q '^starting game$' "$match_dir/server.log" 2>/dev/null; then
            order_checked=1
            if ! validate_player_order "$match_dir/server.log" "$red_algo" "$blue_algo"; then
                clear_progress_line
                echo "[remote-server] #$match_index wrong connection order; marking invalid"
                echo "invalid_player_order" > "$match_dir/status.log"
                status=126
                break
            fi
        fi

        if server_final_seen "$match_dir/server.log"; then
            status=0
            break
        fi

        if (( now - last_repetition >= 1 )); then
            last_repetition="$now"
            if detect_draw_by_repetition "$match_dir"; then
                status=125
                break
            fi
        fi

        if (( now >= deadline )); then
            status=124
            break
        fi

        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            status=127
            break
        fi

        sleep 0.2
    done

    clear_progress_line
    case "$status" in
        0)   echo "ok" > "$match_dir/status.log" ;;
        124) echo "[remote-server] #$match_index timed out after ${MATCH_TIMEOUT}s" | tee "$match_dir/status.log" ;;
        125) echo "[remote-server] #$match_index draw by repetition" | tee "$match_dir/status.log" ;;
        126) ;;
        *)   echo "[remote-server] #$match_index server stopped unexpectedly" | tee "$match_dir/status.log" ;;
    esac

    cleanup_server
    SERVER_PID=""

    record_match_score "$side" "$match_dir"
    echo "[remote-server] #$match_index done score=${SCORE_A}-${SCORE_B}-${SCORE_DRAWS} unknown=${SCORE_UNKNOWN}"
    sleep 1
}

write_plan

echo "[remote-server] run: $RUN_ID"
echo "[remote-server] dir: $RUN_DIR"
echo "[remote-server] client commands written to: $RUN_DIR/remote_client_commands.txt"
cat "$RUN_DIR/remote_client_commands.txt"

match_index=1
for ((i = 1; i <= GAMES_PER_SIDE; ++i)); do
    run_match "$match_index" "A_red" "$ALGO_A" "$ALGO_B"
    match_index=$((match_index + 1))
done

for ((i = 1; i <= GAMES_PER_SIDE; ++i)); do
    run_match "$match_index" "A_blue" "$ALGO_B" "$ALGO_A"
    match_index=$((match_index + 1))
done

trap - EXIT INT TERM
cleanup_server

echo "[remote-server] done: $RUN_DIR"
