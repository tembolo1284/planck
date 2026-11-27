#!/usr/bin/env bash
# Checks the scripts around the build without building anything: that each
# one parses, prints its help, and turns bad input away.
#
#   scripts_smoke.sh <repository>
set -u

root="${1:?usage: scripts_smoke.sh <repository>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() {
    echo "FAIL: $*"
    exit 1
}

for script in "$root/build.sh" "$root"/scripts/*.sh "$root"/tests/e2e/*.sh "$root"/capi/tests/*.sh; do
    bash -n "$script" || fail "$script does not parse"
done

# ---- build.sh ----

"$root/build.sh" help > "$work/help" 2>&1 || fail "build.sh help failed"
for word in build test rebuild clean bench list --gcc --clang --release --debug --checked \
            --asan --tsan --jobs --target --filter --label EXAMPLES; do
    grep -q -- "$word" "$work/help" || fail "the help menu does not mention $word"
done
"$root/build.sh" --help > "$work/help2" 2>&1 || fail "build.sh --help failed"
cmp -s "$work/help" "$work/help2" || fail "help and --help differ"

"$root/build.sh" list > "$work/list" 2>&1 || fail "build.sh list failed"
[ "$(grep -c '^\(gcc\|clang\)-' "$work/list")" -eq 10 ] || fail "list must show ten configurations"

# Every configuration the script can name must exist as a preset.
for compiler in gcc clang; do
    for kind in release debug checked asan tsan; do
        grep -q "\"name\": \"${compiler}-${kind}\"" "$root/CMakePresets.json" ||
            fail "CMakePresets.json has no ${compiler}-${kind}"
    done
done

refused() {
    "$root/build.sh" "$@" > "$work/refused" 2>&1
    [ $? -eq 2 ] || fail "build.sh $* should have been refused"
    grep -q "help" "$work/refused" || fail "a refusal must point at the help"
}
refused frobnicate
refused --type fast
refused --compiler msvc
refused --jobs many
refused --jobs
refused test --nonsense

# ---- the others ----

"$root/scripts/check_all.sh" --help | grep -q -- "--quick" || fail "check_all.sh --help is missing"
"$root/scripts/bench.sh" --help | grep -q -- "--venue" || fail "bench.sh --help is missing"
"$root/scripts/bench.sh" --nonsense > /dev/null 2>&1
[ $? -eq 2 ] || fail "bench.sh should refuse an unknown option"
"$root/scripts/check_host.sh" > "$work/host" 2>&1 || fail "check_host.sh must always exit 0"
grep -q "cycle counter" "$work/host" || fail "check_host.sh printed no report"

echo "PASS"
