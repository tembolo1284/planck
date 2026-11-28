#!/usr/bin/env bash
# check_all.sh: build and test planck in every configuration that matters.
#
#   scripts/check_all.sh            the usual six
#   scripts/check_all.sh --quick    gcc-checked and clang-checked only
#   scripts/check_all.sh --full     all ten, debug builds included
#   scripts/check_all.sh --gcc      leave Clang out (or --clang, the reverse)
#
# Anything else is handed to build.sh, for example -j 8 or --cxx g++-16.
# One configuration failing does not stop the others. The summary at the end
# says which passed, and the exit status is 0 only if all did.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Both compilers optimised and checked, and each sanitizer once, with GCC.
# The Clang sanitizer builds need Clang's runtime library installed
# (libclang-rt-dev on Debian and Ubuntu) and are part of --full only.
configurations=(gcc-release gcc-checked gcc-asan gcc-tsan clang-release clang-checked)
only=""
passthrough=()

while [ $# -gt 0 ]; do
    case "$1" in
        --quick) configurations=(gcc-checked clang-checked) ;;
        --full)  configurations=(gcc-release gcc-debug gcc-checked gcc-asan gcc-tsan
                                 clang-release clang-debug clang-checked clang-asan clang-tsan) ;;
        --gcc)   only="gcc" ;;
        --clang) only="clang" ;;
        -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) passthrough+=("$1") ;;
    esac
    shift
done

if [ -n "$only" ]; then
    kept=()
    for configuration in "${configurations[@]}"; do
        [ "${configuration%%-*}" = "$only" ] && kept+=("$configuration")
    done
    configurations=("${kept[@]}")
fi

failed=0
for configuration in "${configurations[@]}"; do
    compiler="${configuration%%-*}"
    kind="${configuration#*-}"
    log="$(mktemp)"
    started=$SECONDS
    printf '%-16s ' "$configuration"
    if "$root/build.sh" test "--${compiler}" "--${kind}" ${passthrough[@]+"${passthrough[@]}"} > "$log" 2>&1; then
        summary="$(grep -E 'tests passed' "$log" | tail -n 1)"
        # Say which compiler it really was: the build directory knows.
        used="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' \
            "$root/build/$configuration/CMakeCache.txt" 2>/dev/null | head -n 1)"
        echo "ok      $((SECONDS - started))s   ${summary}   ${used}"
    else
        echo "FAILED  $((SECONDS - started))s"
        grep -E 'error:|\*\*\*Failed|\*\*\*Timeout|tests failed|build.sh:' "$log" | head -n 20 | sed 's/^/    /'
        echo "    full log: $log"
        failed=$((failed + 1))
        continue
    fi
    rm -f "$log"
done

echo
if [ "$failed" -eq 0 ]; then
    echo "all ${#configurations[@]} configurations passed"
    exit 0
fi
echo "${failed} of ${#configurations[@]} configurations failed"
exit 1
