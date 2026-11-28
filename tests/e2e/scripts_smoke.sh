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

# A build directory must be what its name says. CMake forgets everything a
# preset chose when the compiler changes, so build.sh has to notice and
# configure from scratch. This runs a copy of build.sh against a stand-in
# for cmake that only records what it was asked, in a directory whose
# "build" holds nothing but the settings.
mkdir -p "$work/copy/build/gcc-asan" "$work/bin"
cp "$root/build.sh" "$work/copy/build.sh"
for tool in cmake ninja ctest; do
    printf '#!/bin/sh\necho "$@" >> "%s/asked"\n' "$work" > "$work/bin/$tool"
    chmod +x "$work/bin/$tool"
done
settings() {  # compiler, sanitizers, contracts, build type
    printf 'CMAKE_CXX_COMPILER:STRING=%s\nCMAKE_C_COMPILER:STRING=/usr/bin/gcc\n' "$1"
    printf 'PLANCK_SANITIZE:STRING=%s\nPLANCK_CONTRACTS:STRING=%s\n' "$2" "$3"
    printf 'CMAKE_BUILD_TYPE:STRING=%s\nPLANCK_NATIVE:BOOL=OFF\n' "$4"
}
configured() {  # the options to build.sh, after the settings on stdin
    cat > "$work/copy/build/gcc-asan/CMakeCache.txt"
    rm -f "$work/asked"
    PATH="$work/bin:$PATH" "$work/copy/build.sh" build --asan "$@" > "$work/copy.log" 2>&1 ||
        fail "build.sh failed against a stand-in cmake: $(cat "$work/copy.log")"
    grep -- "--preset gcc-asan" "$work/asked" || true
}
afresh() { configured "$@" | grep -q -- "--fresh"; }
here="$(command -v sh)"

settings /usr/bin/g++ "address,undefined" checked RelWithDebInfo | afresh &&
    fail "a build directory that is as it should be was thrown away"
settings /usr/bin/g++ "" auto Release | afresh ||
    fail "a directory named for a sanitizer and built without one was accepted"
settings /usr/bin/g++ "thread" checked RelWithDebInfo | afresh ||
    fail "a directory named for one sanitizer and built with another was accepted"
settings /usr/bin/g++ "address,undefined" auto RelWithDebInfo | afresh ||
    fail "a sanitizer build without its contracts was accepted"
settings /usr/bin/g++ "address,undefined" checked RelWithDebInfo | afresh --cxx "$here" ||
    fail "a new compiler was given to an old build directory without starting it afresh"
settings "$here" "address,undefined" checked RelWithDebInfo | afresh --cxx "$(basename "$here")" &&
    fail "the same compiler by its short name was taken for another"
settings "$here" "address,undefined" checked RelWithDebInfo | afresh --cxx "$here" &&
    fail "the same compiler was taken for another"

# ---- the others ----

"$root/scripts/check_all.sh" --help | grep -q -- "--quick" || fail "check_all.sh --help is missing"
"$root/scripts/bench.sh" --help | grep -q -- "--venue" || fail "bench.sh --help is missing"
"$root/scripts/bench.sh" --nonsense > /dev/null 2>&1
[ $? -eq 2 ] || fail "bench.sh should refuse an unknown option"
"$root/scripts/check_host.sh" > "$work/host" 2>&1 || fail "check_host.sh must always exit 0"
grep -q "cycle counter" "$work/host" || fail "check_host.sh printed no report"

echo "PASS"
