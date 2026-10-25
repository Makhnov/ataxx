#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat >&2 <<'USAGE'
Usage:
  ./scripts/blobwar_remote_player.sh KEY=value ...

Required tournament mode:
  SERVER=<server-host-or-ip>
  ALGO=<my executable/algo id>
  OPP=<opponent algo id>
  PAIR=A|B
  MAP=standard|X
  GAMES_PER_SIDE=<n>
  RUN_ID=<server run id>

Optional:
  PORT=2001
  LOCAL_LOG_ROOT=<repo>/logs/tournament/runs
  BLUE_DELAY=1.25
  RETRY_DELAY=1
  START_INDEX=1
  END_INDEX=<2*n>
  MATCH_TIMEOUT=420
  LOG_FILES=1

One-off mode is also possible:
  COLOR=red|blue RED_ALGO=<red id> BLUE_ALGO=<blue id> MATCH_INDEX=1

Examples:
  ./scripts/blobwar_remote_player.sh SERVER=10.0.0.12 PORT=2002 \
      ALGO=NMP_TT_SF_v1 OPP=NMP_TT_SF_v4 PAIR=A MAP=standard \
      GAMES_PER_SIDE=10 RUN_ID=20260420_remote

  ./scripts/blobwar_remote_player.sh SERVER=10.0.0.12 PORT=2002 \
      ALGO=NMP_TT_SF_v4 OPP=NMP_TT_SF_v1 PAIR=B MAP=standard \
      GAMES_PER_SIDE=10 RUN_ID=20260420_remote
USAGE
}

die() {
    echo "[remote-player] ERROR: $*" >&2
    exit 1
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MM0_DIR="$ROOT_DIR/MM0"
LOG_ROOT="$ROOT_DIR/logs/tournament"
RUNS_DIR="$LOG_ROOT/runs"

SERVER=""
PORT="2001"
ALGO=""
OPP=""
PAIR=""
MAP_NAME="standard"
GAMES_PER_SIDE=""
RUN_ID=""
LOCAL_LOG_ROOT="$RUNS_DIR"
BLUE_DELAY="15"
RETRY_DELAY="1"
START_INDEX="1"
END_INDEX=""
MATCH_TIMEOUT="420"
LOG_FILES="1"
COLOR=""
RED_ALGO=""
BLUE_ALGO=""
MATCH_INDEX=""

for arg in "$@"; do
    case "$arg" in
        *=*)
            key="${arg%%=*}"
            value="${arg#*=}"
            key_lc="$(printf '%s' "$key" | tr '[:upper:]' '[:lower:]')"
            case "$key_lc" in
                server|host) SERVER="$value" ;;
                port) PORT="$value" ;;
                algo|exe) ALGO="$value" ;;
                opp|opponent) OPP="$value" ;;
                pair) PAIR="$value" ;;
                map) MAP_NAME="$value" ;;
                games_per_side|games|n) GAMES_PER_SIDE="$value" ;;
                run_id|runid) RUN_ID="$value" ;;
                upload|upload_to) echo "[remote-player] ignoring obsolete option: $arg" >&2 ;;
                local_log_root|local_logs) LOCAL_LOG_ROOT="$value" ;;
                blue_delay|bluedelay) BLUE_DELAY="$value" ;;
                retry_delay|retry) RETRY_DELAY="$value" ;;
                start_index|start) START_INDEX="$value" ;;
                end_index|end) END_INDEX="$value" ;;
                match_timeout|timeout) MATCH_TIMEOUT="$value" ;;
                log_files|logfiles) LOG_FILES="$value" ;;
                color|colour) COLOR="$value" ;;
                red_algo|red) RED_ALGO="$value" ;;
                blue_algo|blue) BLUE_ALGO="$value" ;;
                match_index|match) MATCH_INDEX="$value" ;;
                help) usage; exit 0 ;;
                *) echo "[remote-player] ignoring unknown option: $arg" >&2 ;;
            esac
            ;;
        -h|--help|help)
            usage
            exit 0
            ;;
        *)
            echo "[remote-player] ignoring argument without KEY=value form: $arg" >&2
            ;;
    esac
done

INPUT_COLOR="$COLOR"
INPUT_RED_ALGO="$RED_ALGO"
INPUT_BLUE_ALGO="$BLUE_ALGO"

[[ -n "$SERVER" ]] || die "SERVER= is required"
[[ -n "$ALGO" ]] || die "ALGO= is required"
[[ "$PORT" =~ ^[0-9]+$ ]] || die "PORT must be numeric"

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

match_label() {
    local match_index="$1"
    local red_algo="$2"
    local blue_algo="$3"
    printf 'match_%03d_%s_RED__%s_BLUE' "$match_index" "$red_algo" "$blue_algo"
}

compute_match_plan() {
    local idx="$1"

    if [[ -n "$INPUT_COLOR" ]]; then
        COLOR="$(printf '%s' "$INPUT_COLOR" | tr '[:upper:]' '[:lower:]')"
        [[ "$COLOR" == "red" || "$COLOR" == "blue" ]] || die "COLOR must be red or blue"
        RED_ALGO="$INPUT_RED_ALGO"
        BLUE_ALGO="$INPUT_BLUE_ALGO"
        [[ -n "$RED_ALGO" ]] || RED_ALGO="$ALGO"
        [[ -n "$BLUE_ALGO" ]] || BLUE_ALGO="${OPP:-unknown}"
        return 0
    fi

    [[ -n "$OPP" ]] || die "OPP= is required in tournament mode"
    [[ -n "$PAIR" ]] || die "PAIR=A|B is required in tournament mode"
    [[ -n "$GAMES_PER_SIDE" ]] || die "GAMES_PER_SIDE= is required in tournament mode"
    [[ "$GAMES_PER_SIDE" =~ ^[0-9]+$ ]] || die "GAMES_PER_SIDE must be numeric"

    local pair_uc
    pair_uc="$(printf '%s' "$PAIR" | tr '[:lower:]' '[:upper:]')"
    [[ "$pair_uc" == "A" || "$pair_uc" == "B" ]] || die "PAIR must be A or B"

    if (( idx <= GAMES_PER_SIDE )); then
        if [[ "$pair_uc" == "A" ]]; then
            COLOR="red"
            RED_ALGO="$ALGO"
            BLUE_ALGO="$OPP"
        else
            COLOR="blue"
            RED_ALGO="$OPP"
            BLUE_ALGO="$ALGO"
        fi
    else
        if [[ "$pair_uc" == "A" ]]; then
            COLOR="blue"
            RED_ALGO="$OPP"
            BLUE_ALGO="$ALGO"
        else
            COLOR="red"
            RED_ALGO="$ALGO"
            BLUE_ALGO="$OPP"
        fi
    fi
}

run_one_match() {
    local idx="$1"
    COLOR=""
    RED_ALGO=""
    BLUE_ALGO=""
    compute_match_plan "$idx"

    local label
    label="$(match_label "$idx" "$RED_ALGO" "$BLUE_ALGO")"
    local local_run_dir="$LOCAL_LOG_ROOT/$RUN_ID"
    local match_dir="$local_run_dir/$label"
    mkdir -p "$match_dir"

    local color_log="$match_dir/${COLOR}.log"
    local attempts_log="$match_dir/${COLOR}.attempts.log"
    local exe_path
    exe_path="$(resolve_exe "$ALGO")"

    echo "[remote-player] match #$idx role=$COLOR algo=$ALGO vs=$OPP server=$SERVER:$PORT map=$MAP_NAME"

    local attempt=0
    while true; do
        attempt=$((attempt + 1))
        echo "[remote-player] attempt $attempt for $label" | tee -a "$attempts_log"

        if [[ "$COLOR" == "blue" ]]; then
            sleep "$BLUE_DELAY"
        fi

        set +e
        (
            cd "$MM0_DIR"
            timeout "$MATCH_TIMEOUT" "$exe_path" \
                SERVER="$SERVER" \
                PORT="$PORT" \
                MAP="$MAP_NAME" \
                TEAM="$ALGO" \
                ALGO="$ALGO" \
                ALGO_NAME="$ALGO" \
                COLOR="$COLOR" \
                LOG_DIR="$match_dir" \
                LOG_FILES="$LOG_FILES"
        ) >>"$color_log" 2>&1
        status=$?
        set -e

        if grep -q 'on_start:' "$color_log" 2>/dev/null ||
           grep -q '\[MM0GAME\]' "$color_log" 2>/dev/null ||
           compgen -G "$match_dir/raw/${ALGO}.*.csv" >/dev/null; then
            echo "[remote-player] match #$idx finished or server closed; status=$status"
            break
        fi

        echo "[remote-player] server not ready yet or connection failed; retry in ${RETRY_DELAY}s"
        sleep "$RETRY_DELAY"
    done
}

if [[ -z "$RUN_ID" ]]; then
    RUN_ID="$(date +%Y%m%d_%H%M%S)_${ALGO}_remote_player"
fi

if [[ -n "$MATCH_INDEX" ]]; then
    START_INDEX="$MATCH_INDEX"
    END_INDEX="$MATCH_INDEX"
elif [[ -z "$END_INDEX" ]]; then
    [[ -n "$GAMES_PER_SIDE" ]] || die "GAMES_PER_SIDE= is required unless MATCH_INDEX= is set"
    END_INDEX=$((GAMES_PER_SIDE * 2))
fi

[[ "$START_INDEX" =~ ^[0-9]+$ ]] || die "START_INDEX must be numeric"
[[ "$END_INDEX" =~ ^[0-9]+$ ]] || die "END_INDEX must be numeric"
(( START_INDEX <= END_INDEX )) || die "START_INDEX must be <= END_INDEX"

echo "[remote-player] local logs: $LOCAL_LOG_ROOT/$RUN_ID"

for ((idx = START_INDEX; idx <= END_INDEX; ++idx)); do
    run_one_match "$idx"
done

echo "[remote-player] done"
