#!/usr/bin/env bash
# The shared library must offer exactly the functions in planck.h, and must
# not drag the C++ runtime into a C program.
#
#   capi_exports.sh <libplanck.so> <planck.h> <check-runtime: yes|no>
set -u

library="${1:?usage: capi_exports.sh <library> <header> <yes|no>}"
header="${2:?}"
check_runtime="${3:?}"

fail() {
    echo "FAIL: $*"
    exit 1
}

exported="$(nm -D --defined-only "$library" | awk '{print $3}' | sed 's/@.*//' | sort -u)"
declared="$(grep -o 'planck_[a-z_]*(' "$header" | tr -d '(' | sort -u)"

[ -n "$exported" ] || fail "the library exports nothing"
if [ "$exported" != "$declared" ]; then
    echo "exported by the library:"; echo "$exported"
    echo "declared in the header:"; echo "$declared"
    fail "the library and the header disagree"
fi

if [ "$check_runtime" = "yes" ]; then
    if ldd "$library" | grep -Eq 'libstdc\+\+|libc\+\+'; then
        ldd "$library"
        fail "the library needs the C++ runtime"
    fi
fi
echo "PASS"
