#!/bin/sh
# Shared helpers for scripts/*.sh
#
# Usage pattern in each script:
#   ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
#   . "$ROOT_DIR/scripts/common.sh"
#   resolve_libwebp_tools
#
# Optional env:
#   LIBWEBP_BIN_DIR=/path/to/libwebp/examples

set -eu

note() { printf "%s\n" "$*" >&2; }
die() { note "error: $*"; exit 2; }

# Resolve a tool by name into a variable.
# Search order:
#   1) ${LIBWEBP_BIN_DIR}/<tool>
#   2) <repo>/../../libwebp/examples/<tool>
#   3) <repo>/../libwebp/examples/<tool> (common mono-repo layout)
#   4) PATH
resolve_tool() {
	var_name=$1
	tool_name=$2

	if [ -z "${ROOT_DIR:-}" ]; then
		die "ROOT_DIR is not set; scripts must set ROOT_DIR before sourcing common.sh"
	fi

	candidates=""
	if [ -n "${LIBWEBP_BIN_DIR:-}" ]; then
		candidates="$candidates ${LIBWEBP_BIN_DIR%/}/$tool_name"
	fi
	candidates="$candidates $ROOT_DIR/../../libwebp/examples/$tool_name"
	candidates="$candidates $ROOT_DIR/../libwebp/examples/$tool_name"

	for p in $candidates; do
		if [ -x "$p" ]; then
			eval "$var_name=\"$p\""
			return 0
		fi
	done

	if command -v "$tool_name" >/dev/null 2>&1; then
		p=$(command -v "$tool_name")
		eval "$var_name=\"$p\""
		return 0
	fi

	# Leave unset; caller can decide whether to require.
	return 1
}

resolve_libwebp_tools() {
	resolve_tool WEBPINFO webpinfo || true
	resolve_tool DWEBP dwebp || true
	resolve_tool CWEBP cwebp || true
}

require_tool() {
	var_name=$1
	tool_desc=$2
	val=$(eval "printf %s \"\${$var_name:-}\"")
	if [ -z "$val" ] || [ ! -x "$val" ]; then
		die "$tool_desc not found; set LIBWEBP_BIN_DIR to your libwebp examples dir"
	fi
}

require_libwebp_webpinfo() {
	resolve_libwebp_tools
	require_tool WEBPINFO "webpinfo"
}

require_libwebp_dwebp() {
	resolve_libwebp_tools
	require_tool DWEBP "dwebp"
}

require_libwebp_cwebp() {
	resolve_libwebp_tools
	require_tool CWEBP "cwebp"
}

artifact_base_dir() {
	if [ -z "${ROOT_DIR:-}" ]; then
		die "ROOT_DIR is not set; scripts must set ROOT_DIR before sourcing common.sh"
	fi

	script_name=$(basename "$0")
	script_base=${script_name%.sh}
	dir="$ROOT_DIR/build/test-artifacts/$script_base"
	mkdir -p "$dir"
	printf "%s" "$dir"
}

mk_artifact_tmpdir() {
	dir=$(artifact_base_dir)
	mktemp -d "$dir/tmp.XXXXXX"
}

mk_artifact_tmpfile() {
	dir=$(artifact_base_dir)
	mktemp "$dir/tmp.XXXXXX"
}
