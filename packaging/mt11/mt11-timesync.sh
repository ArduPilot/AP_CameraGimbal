#!/bin/sh

SERVER=192.168.144.73
PIDFILE=/run/mt11-timesync.pid

if [ -r "$PIDFILE" ]; then
	old_pid=$(cat "$PIDFILE")
	if kill -0 "$old_pid" 2>/dev/null; then
		exit 0
	fi
fi

echo $$ >"$PIDFILE"

# product_upgrade sends SIGTERM to every process whose command contains
# /app and treats a survivor after 60 retries as fatal.  The old combined
# EXIT/TERM trap removed the pidfile but then resumed this loop, forcing the
# updater to reboot without flashing.  Signal traps must terminate the shell.
trap 'exit 0' HUP INT TERM
trap 'rm -f "$PIDFILE"' EXIT

# BusyBox ash defers a signal trap while it waits for an external command.
# Bound every child wait to five seconds so product_upgrade's SIGTERM is
# handled well before its 60-second deadline.
while ! timeout 5 rdate -s "$SERVER"; do
	sleep 5
done
date -u

elapsed=0
while sleep 5; do
	elapsed=$((elapsed + 5))
	if [ "$elapsed" -ge 900 ]; then
		if timeout 5 rdate -s "$SERVER"; then
			date -u
		fi
		elapsed=0
	fi
done
