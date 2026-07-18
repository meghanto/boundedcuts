#!/usr/bin/env bash
set -euo pipefail

destination="${1:-/tmp/cutwidth-pb-sat}"
mkdir -p "$destination"

clone_pin() {
    local url="$1"
    local commit="$2"
    local directory="$3"
    if [[ ! -d "$directory/.git" ]]; then
        git clone "$url" "$directory"
    fi
    git -C "$directory" fetch --quiet origin "$commit"
    git -C "$directory" checkout --detach --quiet "$commit"
    test "$(git -C "$directory" rev-parse HEAD)" = "$commit"
}

kissat_commit=77bc7ea68afe80751a67df8561357f193e160fb1
cadical_commit=f13d74439a5b5c963ac5b02d05ce93a8098018b8
drat_trim_commit=2e3b2dc0ecf938addbd779d42877b6ed69d9a985

clone_pin https://github.com/arminbiere/kissat.git "$kissat_commit" "$destination/kissat"
clone_pin https://github.com/arminbiere/cadical.git "$cadical_commit" "$destination/cadical"
clone_pin https://github.com/marijnheule/drat-trim.git "$drat_trim_commit" "$destination/drat-trim"

if [[ ! -x "$destination/kissat/build/kissat" ]]; then
    (cd "$destination/kissat" && ./configure && make -j4)
fi
if [[ ! -x "$destination/cadical/build/cadical" ]]; then
    (cd "$destination/cadical" && ./configure && make -j4)
fi
if [[ ! -x "$destination/drat-trim/drat-trim" ]]; then
    make -C "$destination/drat-trim" -j4
fi

test "$($destination/kissat/build/kissat --version)" = 4.0.3
test "$($destination/cadical/build/cadical --version)" = 2.1.3

printf '%s\n' \
    "KISSAT=$destination/kissat/build/kissat" \
    "CADICAL=$destination/cadical/build/cadical" \
    "DRAT_TRIM=$destination/drat-trim/drat-trim" \
    "KISSAT_COMMIT=$kissat_commit" \
    "CADICAL_COMMIT=$cadical_commit" \
    "DRAT_TRIM_COMMIT=$drat_trim_commit"
