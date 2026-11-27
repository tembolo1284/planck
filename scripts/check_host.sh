#!/usr/bin/env bash
# check_host.sh: is this machine set up for low and steady latency?
#
#   scripts/check_host.sh
#
# Reads kernel settings and prints one line for each thing that decides
# whether a latency figure taken here can be trusted. It changes nothing and
# needs no privileges. "ok" is good, "warn" will cost you tail latency, and
# "info" is something to know.
#
# Nothing here matters for correctness. planck runs and passes its tests on
# a laptop. This is about whether its numbers mean anything.
set -u

warnings=0
line() { printf '%-5s %-24s %s\n' "$1" "$2" "$3"; }
ok()   { line ok "$1" "$2"; }
info() { line info "$1" "$2"; }
warn() { line warn "$1" "$2"; warnings=$((warnings + 1)); }
read_file() { cat "$1" 2>/dev/null || true; }

cpus="$(nproc 2>/dev/null || echo '?')"
info "cpus" "${cpus} online: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"

if grep -qi microsoft /proc/version 2>/dev/null; then
    warn "virtualisation" "WSL. The clock and the scheduler belong to Windows (W-30)."
elif grep -q '^flags.* hypervisor' /proc/cpuinfo 2>/dev/null; then
    warn "virtualisation" "a virtual machine. Another tenant can take the core at any time."
else
    ok "virtualisation" "bare metal"
fi

isolated="$(read_file /sys/devices/system/cpu/isolated)"
if [ -n "$isolated" ]; then
    ok "isolated cpus" "$isolated. Pin planck's threads there."
else
    warn "isolated cpus" "none. Boot with isolcpus=<list> to keep other work off planck's cores."
fi

nohz="$(read_file /sys/devices/system/cpu/nohz_full)"
if [ -n "$nohz" ] && [ "$nohz" != "(null)" ]; then
    ok "tickless cpus" "$nohz"
else
    warn "tickless cpus" "none. Boot with nohz_full=<list> to stop the timer tick on those cores."
fi

governors="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ')"
case "$governors" in
    "")             info "frequency governor" "not exposed" ;;
    "performance ") ok "frequency governor" "performance" ;;
    *)              warn "frequency governor" "${governors}: set every core to performance" ;;
esac

smt="$(read_file /sys/devices/system/cpu/smt/active)"
case "$smt" in
    0) ok "hyperthreading" "off" ;;
    1) warn "hyperthreading" "on. Two threads share a core: pin to one of each pair, or turn it off." ;;
    *) info "hyperthreading" "unknown" ;;
esac

if grep -q '^flags.* constant_tsc' /proc/cpuinfo 2>/dev/null &&
   grep -q '^flags.* nonstop_tsc' /proc/cpuinfo 2>/dev/null; then
    ok "cycle counter" "constant and nonstop"
else
    warn "cycle counter" "not marked constant and nonstop. planck's timestamps rely on it."
fi

clocksource="$(read_file /sys/devices/system/clocksource/clocksource0/current_clocksource)"
case "$clocksource" in
    tsc) ok "clock source" "tsc" ;;
    "")  info "clock source" "unknown" ;;
    *)   warn "clock source" "${clocksource}. Reading the time is a slow path here. tsc is the fast one." ;;
esac

huge_total="$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo 2>/dev/null)"
huge_free="$(awk '/^HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null)"
if [ "${huge_total:-0}" -gt 0 ]; then
    ok "huge pages" "${huge_free} of ${huge_total} free (2 MiB each)"
else
    warn "huge pages" "none reserved. planck falls back to 4 KiB pages. See vm.nr_hugepages."
fi

thp="$(read_file /sys/kernel/mm/transparent_hugepage/enabled)"
case "$thp" in
    *"[always]"*) warn "transparent huge pages" "always. The kernel may stall a thread to compact memory. Prefer madvise." ;;
    "")           info "transparent huge pages" "unknown" ;;
    *)            ok "transparent huge pages" "$(echo "$thp" | sed 's/.*\[\(.*\)\].*/\1/')" ;;
esac

swap="$(awk 'NR > 1 {total += $3} END {print total + 0}' /proc/swaps 2>/dev/null)"
if [ "${swap:-0}" -gt 0 ]; then
    warn "swap" "on. Run planckd with --lock-memory, or turn swap off."
else
    ok "swap" "off"
fi

memlock="$(ulimit -l 2>/dev/null)"
if [ "$memlock" = "unlimited" ]; then
    ok "locked memory limit" "unlimited"
else
    info "locked memory limit" "${memlock} KiB. planckd --lock-memory needs this raised."
fi

rtprio="$(ulimit -r 2>/dev/null)"
if [ "${rtprio:-0}" != "0" ]; then
    ok "realtime priority" "allowed up to ${rtprio}"
else
    info "realtime priority" "not allowed for this user. Only matters if you ask for it."
fi

if pgrep -x irqbalance > /dev/null 2>&1; then
    warn "irqbalance" "running. It moves interrupts onto any core, isolated ones included."
else
    ok "irqbalance" "not running"
fi

echo
if [ "$warnings" -eq 0 ]; then
    echo "This machine is set up for latency measurement."
else
    echo "${warnings} warnings. Numbers taken here show where the time goes, not how fast planck is."
fi
exit 0
