#!/usr/bin/env bash
# Launch `bun prof:js markdown_demo` and terminate the whole process tree
# (bun -> scripts/js.ts -> fxe_run) after DURATION seconds.
#
# After SIGINT the bun parent exits within ~50 ms but fxe_run still needs
# several seconds to stop the V8 sampler, merge profiles, and write
# fxe.cpuprofile + fxe.md. We therefore wait for the entire process group
# (not just the bun pid) to drain before returning, otherwise the script
# returns "successfully" while fxe_run is still flushing and the caller
# sees no fxe.md.
set -u

DURATION="${1:-20}"

cd "$(dirname "$0")/.."

# Job control gives the backgrounded pipeline its own process group so we
# can signal bun + every descendant (notably fxe_run) at once via
# `kill -- -PGID`.
set -m
bun prof:js markdown_demo &
pid=$!
set +m

signal_group() {
  local sig=$1
  # Negative pid targets the process group. Fall back to plain pid if the
  # group is already gone.
  kill -"$sig" -- -"$pid" 2>/dev/null || kill -"$sig" "$pid" 2>/dev/null || true
}

# True while ANY descendant of the launcher is still alive — fxe_run flushes
# the CPU profile after bun has already exited, so we cannot rely on the
# bun pid alone.
group_alive() {
  pgrep -g "$pid" >/dev/null 2>&1
}

cleanup() {
  if kill -0 "$pid" 2>/dev/null || group_alive; then
    # SIGINT lets fxe_run flush the cpu profile cleanly.
    signal_group INT
    # Give the group up to 30 s to drain (CPU profile flush can take a
    # few seconds on large traces). Polling stops as soon as the last
    # member exits.
    for _ in $(seq 1 300); do
      group_alive || break
      sleep 0.1
    done
    if group_alive; then
      signal_group TERM
      for _ in $(seq 1 30); do
        group_alive || break
        sleep 0.1
      done
    fi
    if group_alive; then
      signal_group KILL
    fi
  fi
  wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep "$DURATION"
cleanup
trap - EXIT
