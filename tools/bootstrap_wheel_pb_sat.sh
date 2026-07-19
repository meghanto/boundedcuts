#!/usr/bin/env bash
set -euo pipefail

destination="${1:?usage: bootstrap_wheel_pb_sat.sh DESTINATION [JOBS]}"
jobs="${2:-4}"
cadical_commit=f13d74439a5b5c963ac5b02d05ce93a8098018b8
drat_trim_commit=2e3b2dc0ecf938addbd779d42877b6ed69d9a985

mkdir -p "$destination"

clone_pin() {
    local url="$1" commit="$2" directory="$3"
    if [[ ! -d "$directory/.git" ]]; then
        git clone --filter=blob:none "$url" "$directory"
    fi
    test -z "$(git -C "$directory" status --porcelain --untracked-files=no)"
    git -C "$directory" fetch --quiet origin "$commit"
    git -C "$directory" checkout --detach --quiet "$commit"
    test "$(git -C "$directory" rev-parse HEAD)" = "$commit"
}

clone_pin https://github.com/arminbiere/cadical.git "$cadical_commit" "$destination/cadical"
clone_pin https://github.com/marijnheule/drat-trim.git "$drat_trim_commit" "$destination/drat-trim"

cadical_stamp="$destination/cadical/build/.boundedcuts-commit"
if [[ ! -x "$destination/cadical/build/cadical" || ! -f "$cadical_stamp" ||
      "$(cat "$cadical_stamp" 2>/dev/null || true)" != "$cadical_commit" ]]; then
    rm -rf "$destination/cadical/build"
    (cd "$destination/cadical" && ./configure && make -j"$jobs" cadical)
    printf '%s\n' "$cadical_commit" >"$cadical_stamp"
fi
drat_stamp="$destination/drat-trim/.boundedcuts-build-commit"
if [[ ! -x "$destination/drat-trim/drat-trim" || ! -f "$drat_stamp" ||
      "$(cat "$drat_stamp" 2>/dev/null || true)" != "$drat_trim_commit" ]]; then
    cc "$destination/drat-trim/drat-trim.c" -std=c99 -O2 \
        -Dgetc_unlocked=getc -o "$destination/drat-trim/drat-trim"
    printf '%s\n' "$drat_trim_commit" >"$drat_stamp"
fi

test "$($destination/cadical/build/cadical --version)" = 2.1.3
tmp="$(mktemp -d "${TMPDIR:-/tmp}/boundedcuts-pb.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
printf 'p cnf 1 2\n1 0\n-1 0\n' >"$tmp/contradiction.cnf"
set +e
"$destination/cadical/build/cadical" "$tmp/contradiction.cnf" "$tmp/proof.drat" >/dev/null
status=$?
set -e
test "$status" = 20
"$destination/drat-trim/drat-trim" "$tmp/contradiction.cnf" "$tmp/proof.drat" | grep -q VERIFIED
