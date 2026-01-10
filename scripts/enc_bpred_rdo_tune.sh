#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

sizes_str=${SIZES:-"256"}
qs_str=${QS:-"40 60 80"}
ours_flags=${OURS_FLAGS:-"--loopfilter"}
jobs=${JOBS:-1}

if [ "$#" -gt 0 ]; then
	images=()
	for arg in "$@"; do
		if [ -d "$arg" ]; then
			while IFS= read -r -d '' p; do
				images+=("$p")
			done < <(
				find "$arg" -maxdepth 1 -type f \( \
					-iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \
				\) -print0 | sort -z
			)
		else
			case "$arg" in
				*.jpg|*.jpeg|*.png|*.JPG|*.JPEG|*.PNG) images+=("$arg") ;;
				*) ;; # ignore non-images
			esac
		fi
	done
	if [ "${#images[@]}" -eq 0 ]; then
		die "no input images (pass files or a directory containing .jpg/.png)"
	fi
else
	# Small mixed corpus:
	# - Natural photos: commons
	# - A couple of structured/graphics-heavy images: testimages
	images=(
		"images/commons/crane.jpg"
		"images/commons/penguin.jpg"
		"images/commons/antpilla.jpg"
		"images/testimages/png/whale.png"
		"images/testimages/png/w3c_home_256.png"
	)
fi

run_one() {
	local mode=$1
	JOBS="$jobs" SIZES="$sizes_str" QS="$qs_str" MODE="$mode" OURS_FLAGS="$ours_flags" \
		./scripts/enc_vs_cwebp_quality.sh "${images[@]}"
}

extract_summary() {
	# Print Summary block + Overall + Artifacts.
	awk '
		/^== Summary \(ours - lib\) ==$/ {print; in_summary=1; next}
		in_summary && /^Artifacts:/ {print; exit}
		in_summary {print}
	'
}

extract_overall() {
	awk -F': ' '/^Overall:/ {print $2; exit}'
}

printf '== bpred-rdo tuning harness ==\n'
printf 'Images (%d): %s\n' "${#images[@]}" "${images[*]}"
printf 'JOBS=%s  SIZES=%s  QS=%s  OURS_FLAGS=%s\n\n' "$jobs" "$sizes_str" "$qs_str" "$ours_flags"

printf '== MODE=bpred ==\n'
bpred_out=$(run_one bpred)
printf '%s\n' "$bpred_out" | extract_summary

printf '\n== MODE=bpred-rdo ==\n'
bpred_rdo_out=$(run_one bpred-rdo)
printf '%s\n' "$bpred_rdo_out" | extract_summary

bpred_overall=$(printf '%s\n' "$bpred_out" | extract_overall || true)
bpred_rdo_overall=$(printf '%s\n' "$bpred_rdo_out" | extract_overall || true)

printf '\n== Overall comparison ==\n'
printf 'bpred:     %s\n' "$bpred_overall"
printf 'bpred-rdo: %s\n' "$bpred_rdo_overall"
