#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Check that each module under src/ only includes project headers from
# itself and from the modules it may depend on. See docs/architecture.md,
# "Layers".
#
# Usage: check-layering.sh <src-dir>

set -eu

src=${1:?usage: check-layering.sh <src-dir>}

# Print the modules whose headers a module may include, besides its own.
# Keep this table in sync with docs/architecture.md.
allowed()
{
	case $1 in
	util) echo "" ;;
	core) echo "util" ;;
	backend | cpu | hw) echo "core util" ;;
	boot | debug) echo "backend core util" ;;
	machine) echo "backend boot cpu hw core util" ;;
	frontend) echo "machine backend boot cpu debug hw core util" ;;
	*) return 1 ;;
	esac
}

status=0
newline='
'

for dir in "$src"/*/; do
	module=$(basename "$dir")
	if ! deps=$(allowed "$module"); then
		echo "error: $module: unknown module; add it to $0" >&2
		status=1
		continue
	fi

	# Project headers are included as "module/...", for example
	# #include "core/bus.h".
	matches=$(grep -rnE '^#[[:space:]]*include[[:space:]]+"[a-z0-9_]+/' \
		--include='*.c' --include='*.h' "$dir" || true)
	[ -n "$matches" ] || continue

	# Split the matches into lines, without glob expansion.
	set -f
	IFS=$newline
	for line in $matches; do
		target=$(printf '%s\n' "$line" |
			sed -E 's/.*include[[:space:]]+"([a-z0-9_]+)\/.*/\1/')
		case " $module $deps " in
		*" $target "*) ;;
		*)
			echo "error: $line" >&2
			echo "  $module/ must not include headers from $target/" >&2
			status=1
			;;
		esac
	done
	unset IFS
	set +f
done

exit $status
