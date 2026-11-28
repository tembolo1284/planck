#!/usr/bin/env bash
# build.sh: configure, build, test and clean planck.
#
# A thin front end to the presets in CMakePresets.json. Every combination of
# compiler and build kind gets a directory of its own under build/, so
# switching between them never forces a full rebuild.
#
# Run ./build.sh help for the menu.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command="build"
compiler="gcc"
kind="release"
jobs=""
target=""
filter=""
label=""
cxx=""
cc=""
native="ON"
verbose=0
everything=0
fresh=0
extra=()

if [ -t 1 ]; then
    bold=$'\033[1m'; dim=$'\033[2m'; red=$'\033[31m'; green=$'\033[32m'; plain=$'\033[0m'
else
    bold=""; dim=""; red=""; green=""; plain=""
fi

usage() {
    cat <<EOF
${bold}planck build script${plain}

${bold}USAGE${plain}
    ./build.sh [command] [options] [-- extra arguments]

${bold}COMMANDS${plain}
    build        Configure if needed, then build.                 (default)
    test         Build, then run the tests.
    rebuild      Throw the build directory away and build again.
    clean        Remove this configuration's build directory.
    bench        Build, then run planck-bench. Use a release build.
    list         Show every configuration and whether it has been built.
    help         Show this menu.

${bold}COMPILER${plain}
    --gcc                 Build with GCC.                         (default)
    --clang               Build with Clang.
    -c, --compiler NAME   The same, spelled out: gcc or clang.
    --cxx PATH            Use exactly this C++ compiler, e.g. g++-16.
    --cc PATH             Use exactly this C compiler, e.g. gcc-16.

${bold}BUILD KIND${plain}
    --release             Optimised, contracts assumed.           (default)
                          The one to measure and to run.
    --debug               No optimisation, contracts checked.
                          The one to step through in a debugger.
    --checked             Optimised with debug info, contracts checked.
                          The one to run the tests with.
    --asan                Checked, plus AddressSanitizer and UBSan.
    --tsan                Checked, plus ThreadSanitizer.
    -t, --type KIND       The same, spelled out.

${bold}OPTIONS${plain}
    -j, --jobs N          Parallel jobs for building and testing. (all CPUs)
    --target NAME         Build only this target, e.g. planckd.
    -R, --filter REGEX    test: run only tests whose name matches.
    -L, --label LABEL     test: run only one group: unit, property,
                          integration, e2e or capi.
    --portable            Do not tune for this machine (-march=native off).
    --fresh               Configure again from scratch, keeping nothing.
    --all                 clean: remove every configuration, not just one.
    -v, --verbose         Show the compiler commands, and all test output.
    -h, --help            Show this menu.

    Anything after -- goes to ctest (test), to planck-bench (bench), or to
    the build tool (build).

${bold}EXAMPLES${plain}
    ./build.sh                           GCC release build
    ./build.sh test --checked            the usual way to run the tests
    ./build.sh test --clang --asan       Clang with the address sanitizer
    ./build.sh test --debug -R Shard     only the shard tests, unoptimised
    ./build.sh build --target planckd    just the venue
    ./build.sh bench -- --cpu 2 --cpu2 3 benchmarks, pinned
    ./build.sh --cxx g++-16 --cc gcc-16  a compiler by its exact name
    ./build.sh clean --all               remove everything under build/

${bold}WHERE THINGS GO${plain}
    build/<compiler>-<kind>/             for example build/gcc-release/
        apps/    planckd, planck-client, planck-mdtap, planck-replay
        bench/   planck-bench, planck-loadgen
        capi/    libplanck.so

    The same configurations are in CMakePresets.json, so an editor or a
    plain "cmake --preset gcc-release" lands in the same directory.
EOF
}

die() {
    echo "${red}build.sh: $*${plain}" >&2
    echo "Try ./build.sh help" >&2
    exit 2
}

say() {
    echo "${bold}==> $*${plain}"
}

# The value of an option that takes one, or a complaint if it is missing.
value_of() {
    [ $# -ge 2 ] || die "$1 needs a value"
    printf '%s' "$2"
}

while [ $# -gt 0 ]; do
    case "$1" in
        build|test|rebuild|clean|bench|list|help) command="$1" ;;
        --gcc)            compiler="gcc" ;;
        --clang)          compiler="clang" ;;
        -c|--compiler)    compiler="$(value_of "$@")"; shift ;;
        --cxx)            cxx="$(value_of "$@")"; shift ;;
        --cc)             cc="$(value_of "$@")"; shift ;;
        --release|--debug|--checked|--asan|--tsan) kind="${1#--}" ;;
        -t|--type)        kind="$(value_of "$@")"; shift ;;
        -j|--jobs)        jobs="$(value_of "$@")"; shift ;;
        -j[0-9]*)         jobs="${1#-j}" ;;
        --target)         target="$(value_of "$@")"; shift ;;
        -R|--filter)      filter="$(value_of "$@")"; shift ;;
        -L|--label)       label="$(value_of "$@")"; shift ;;
        --portable)       native="OFF" ;;
        --fresh)          fresh=1 ;;
        --all)            everything=1 ;;
        -v|--verbose)     verbose=1 ;;
        -h|--help)        command="help" ;;
        --)               shift; extra=("$@"); break ;;
        *)                die "unknown argument '$1'" ;;
    esac
    shift
done

case "$compiler" in gcc|clang) ;; *) die "compiler must be gcc or clang, not '$compiler'" ;; esac
case "$kind" in
    release|debug|checked|asan|tsan) ;;
    *) die "build kind must be release, debug, checked, asan or tsan, not '$kind'" ;;
esac
if [ -n "$jobs" ] && ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    die "--jobs needs a number, not '$jobs'"
fi

preset="${compiler}-${kind}"
build_dir="${root}/build/${preset}"
if [ -z "$jobs" ]; then
    jobs="$(nproc 2>/dev/null || echo 4)"
fi

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is not installed or not on PATH"
}

# What the build directory was last configured with, for one setting.
cached() {
    sed -n "s/^$1:[A-Z]*=//p" "${build_dir}/CMakeCache.txt" 2>/dev/null | head -n 1
}

# True if the compiler asked for is not the one the build directory has.
other_compiler() {
    local asked="$1" has="$2"
    [ -n "$asked" ] || return 1
    [ "$has" != "$asked" ] && [ "$has" != "$(command -v "$asked" 2>/dev/null)" ]
}

# True if the build directory exists and is not what its name says.
#
# CMake throws its whole cache away when the compiler changes, and what the
# preset chose goes with it: the build type, the contracts, the sanitizers.
# It then configures again with its own defaults. So build/gcc-asan, given
# another compiler, would quietly become an optimised build with no
# sanitizer, and report every test as passing under AddressSanitizer for
# ever after. Such a directory has to be configured from scratch, which
# applies the preset again. A directory left that way by an older build.sh
# is caught too, by what it holds.
misconfigured() {
    [ -f "${build_dir}/CMakeCache.txt" ] || return 1
    if other_compiler "$cxx" "$(cached CMAKE_CXX_COMPILER)" ||
       other_compiler "$cc" "$(cached CMAKE_C_COMPILER)"; then
        return 0
    fi
    local sanitize="" contracts="auto" build_type="RelWithDebInfo"
    case "$kind" in
        release) build_type="Release" ;;
        debug)   build_type="Debug" ;;
        checked) contracts="checked" ;;
        asan)    contracts="checked"; sanitize="address,undefined" ;;
        tsan)    contracts="checked"; sanitize="thread" ;;
    esac
    [ "$(cached PLANCK_SANITIZE)" != "$sanitize" ] ||
        [ "$(cached PLANCK_CONTRACTS)" != "$contracts" ] ||
        [ "$(cached CMAKE_BUILD_TYPE)" != "$build_type" ]
}

configure() {
    need cmake
    need ninja
    local arguments=(--preset "$preset" "-DPLANCK_NATIVE=${native}")
    [ -n "$cxx" ] && arguments+=("-DCMAKE_CXX_COMPILER=${cxx}")
    [ -n "$cc" ] && arguments+=("-DCMAKE_C_COMPILER=${cc}")
    if [ "$fresh" -eq 0 ] && misconfigured; then
        say "build/${preset} was configured for another compiler or another kind of build: starting it afresh"
        fresh=1
    fi
    [ "$fresh" -eq 1 ] && arguments+=(--fresh)
    # A build directory made by hand, straight in build/, would shadow the
    # ones made here.
    if [ -f "${root}/build/CMakeCache.txt" ]; then
        die "build/ holds an old build made without presets. Remove it first: rm -rf build"
    fi
    # Configure when there is nothing yet, or when asked for something that
    # only a configure can change.
    if [ ! -f "${build_dir}/CMakeCache.txt" ] || [ "$fresh" -eq 1 ] || [ -n "$cxx" ] ||
       [ -n "$cc" ] || ! grep -q "^PLANCK_NATIVE:BOOL=${native}$" "${build_dir}/CMakeCache.txt"; then
        say "configuring ${preset}"
        if [ "$verbose" -eq 1 ]; then
            (cd "$root" && cmake "${arguments[@]}")
        else
            (cd "$root" && cmake "${arguments[@]}" >/dev/null)
        fi
    fi
}

build() {
    configure
    local arguments=(--build "$build_dir" --parallel "$jobs")
    [ -n "$target" ] && arguments+=(--target "$target")
    [ "$verbose" -eq 1 ] && arguments+=(--verbose)
    if [ "$command" = "build" ] && [ ${#extra[@]} -gt 0 ]; then
        arguments+=(-- "${extra[@]}")
    fi
    say "building ${preset}${target:+ (${target})} with ${jobs} jobs"
    cmake "${arguments[@]}"
}

run_tests() {
    local arguments=(--test-dir "$build_dir" --parallel "$jobs")
    if [ "$verbose" -eq 1 ]; then
        arguments+=(--verbose)
    else
        arguments+=(--output-on-failure)
    fi
    [ -n "$filter" ] && arguments+=(-R "$filter")
    [ -n "$label" ] && arguments+=(-L "$label")
    [ ${#extra[@]} -gt 0 ] && arguments+=("${extra[@]}")
    say "testing ${preset}"
    ctest "${arguments[@]}"
}

clean() {
    if [ "$everything" -eq 1 ]; then
        say "removing build/"
        rm -rf "${root}/build"
    else
        say "removing build/${preset}"
        rm -rf "$build_dir"
    fi
}

list() {
    echo "${bold}configuration     built   directory${plain}"
    local c k name state
    for c in gcc clang; do
        for k in release debug checked asan tsan; do
            name="${c}-${k}"
            if [ -f "${root}/build/${name}/CMakeCache.txt" ]; then
                state="${green}yes${plain}  "
            else
                state="${dim}no${plain}   "
            fi
            printf '%-17s %s   build/%s\n' "$name" "$state" "$name"
        done
    done
}

case "$command" in
    help)    usage ;;
    list)    list ;;
    clean)   clean ;;
    build)   build ;;
    rebuild) rm -rf "$build_dir"; build ;;
    test)    build; run_tests ;;
    bench)
        if [ "$kind" != "release" ]; then
            echo "${red}note: ${kind} is not a release build. These numbers will not be the engine's speed.${plain}" >&2
        fi
        target="planck-bench"
        build
        say "running planck-bench"
        "${build_dir}/bench/planck-bench" ${extra[@]+"${extra[@]}"}
        ;;
esac
