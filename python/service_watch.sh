#!/usr/bin/env bash
# service_watch.sh — make sure a service is listening on 127.0.0.1:$PORT on the
# machine this runs on, then stay in the foreground and report on it.
#
# The AI Console runs this for a tool's "Start" button, either directly (local
# service) or through ssh on the remote host (remote service; the same ssh
# connection carries the port forward). Its output is what the app's log shows.
#
# Inputs (environment variables; only PORT is required):
#   NAME        label used in log lines                      (default: service)
#   PORT        TCP port the service listens on, on loopback
#   START_CMD   shell command that starts the service when nothing is listening.
#               It may stay in the foreground or put itself in the background.
#   INFO_PATH   HTTP path that describes the running service, printed once up
#   LOG_FILE    the service's own log file; new lines are copied to this log
#   READY_NOTE  printed once the service is up (e.g. how to reach it)
#   HINT        printed if the service has not come up after a minute
#
# Stopping this script (SIGTERM, or stdin closing because ssh/the app went
# away) never stops the service itself.

NAME=${NAME:-service}
START_CMD=${START_CMD:-}
INFO_PATH=${INFO_PATH:-}
LOG_FILE=${LOG_FILE:-}
READY_NOTE=${READY_NOTE:-}
HINT=${HINT:-}
LOG_FILE=${LOG_FILE/#\~/$HOME}

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

if [ -z "${PORT:-}" ]; then
    log "error: PORT is not set"
    exit 2
fi

bg_pids=""
cleanup() {
    for p in $bg_pids; do kill "$p" 2>/dev/null; done
}
trap cleanup EXIT
trap 'exit 0' TERM INT HUP

listening() { (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; }

# Sleep $1 seconds. Reading stdin is the sleep: when stdin reaches EOF the
# caller (ssh session or the app) is gone, so stop instead of lingering.
pause() {
    local t0=$SECONDS junk
    if read -r -t "$1" junk; then
        return 0                      # a stray line on stdin: ignore it
    fi
    if [ $((SECONDS - t0)) -lt "$1" ]; then
        exit 0                        # returned early without data: EOF
    fi
}

show_info() {
    [ -n "$INFO_PATH" ] || return 0
    command -v curl >/dev/null 2>&1 || return 0
    local info
    info=$(curl -s -m 5 "http://127.0.0.1:$PORT$INFO_PATH" 2>/dev/null)
    [ -n "$info" ] && log "$NAME reports: $info"
}

# Over ssh there is no DISPLAY, so a GUI program could not open its window.
# Borrow the display settings of the logged-in desktop session, if there is one.
import_desktop_env() {
    [ "$(uname -s)" = "Linux" ] || return 0
    if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v systemctl >/dev/null 2>&1; then
        local line
        while IFS= read -r line; do
            case "$line" in
                DISPLAY=*|WAYLAND_DISPLAY=*|XAUTHORITY=*|DBUS_SESSION_BUS_ADDRESS=*) export "$line" ;;
            esac
        done < <(systemctl --user show-environment 2>/dev/null)
    fi
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        log "desktop session found (DISPLAY=${DISPLAY:-none}); a GUI program will open there"
    else
        log "no desktop session found on this machine: a GUI program cannot open a window until someone logs in at its screen"
    fi
}

log "$(hostname): checking $NAME on 127.0.0.1:$PORT"

if [ -n "$LOG_FILE" ]; then
    if [ -e "$LOG_FILE" ]; then
        log "copying new lines from $LOG_FILE"
    else
        log "will copy lines from $LOG_FILE once it exists"
    fi
    tail -n 0 -F "$LOG_FILE" 2>/dev/null &
    bg_pids="$bg_pids $!"
fi

if listening; then
    log "$NAME is already running"
elif [ -z "$START_CMD" ]; then
    log "$NAME is not running, and no start command is configured for it"
    exit 1
else
    import_desktop_env
    out="${TMPDIR:-/tmp}/ai-console-$NAME.out"
    : > "$out"
    log "$NAME is not running. Starting it: $START_CMD"
    if command -v setsid >/dev/null 2>&1; then
        setsid nohup bash -c "$START_CMD" >"$out" 2>&1 </dev/null &
    else
        nohup bash -c "$START_CMD" >"$out" 2>&1 </dev/null &
    fi
    tail -n +1 -F "$out" 2>/dev/null &      # whatever the start command prints
    bg_pids="$bg_pids $!"

    waited=0
    until listening; do
        pause 3
        waited=$((waited + 3))
        if [ "$waited" -eq 15 ] || [ "$waited" -eq 30 ] || [ $((waited % 60)) -eq 0 ]; then
            log "still waiting for $NAME to listen on $PORT (${waited}s)"
        fi
        if [ "$waited" -eq 60 ] && [ -n "$HINT" ]; then
            log "hint: $HINT"
        fi
    done
    log "$NAME is up (took about ${waited}s)"
fi

show_info
[ -n "$READY_NOTE" ] && log "$READY_NOTE"
log "watching $NAME. Stopping this only stops watching; $NAME keeps running."

state=up
while :; do
    pause 5
    if listening; then
        if [ "$state" = down ]; then
            log "$NAME is listening on $PORT again"
            show_info
            state=up
        fi
    elif [ "$state" = up ]; then
        log "$NAME stopped listening on $PORT (was it closed?)"
        state=down
    fi
done
