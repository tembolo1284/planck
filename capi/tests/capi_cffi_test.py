#!/usr/bin/env python3
"""Drive libplanck.so from Python through cffi.

    capi_cffi_test.py <libplanck.so> <planck.h>

Nothing here is written by hand to match the library: the declarations come
straight from planck.h, run through the C preprocessor. So this also checks
that the header is plain enough for a binding generator to read.
"""
import subprocess
import sys

import cffi


def load(library_path, header_path):
    declarations = subprocess.run(
        ["cc", "-E", "-P", "-DPLANCK_CFFI", header_path],
        check=True, capture_output=True, text=True).stdout
    ffi = cffi.FFI()
    ffi.cdef(declarations)
    return ffi, ffi.dlopen(library_path)


def reports(ffi, lib, engine):
    out = []
    report = ffi.new("planck_report*")
    while lib.planck_next_report(engine, report) == 1:
        out.append({name: getattr(report, name) for name in
                    ("kind", "session", "client_order_id", "order_id", "match_id",
                     "price", "qty", "leaves", "reason", "liquidity", "timestamp_ns")})
    return out


def main():
    ffi, lib = load(sys.argv[1], sys.argv[2])

    assert lib.planck_abi_version() == lib.PLANCK_ABI_VERSION
    assert ffi.string(lib.planck_version()).count(b".") == 2
    assert ffi.sizeof("planck_order") == 32
    assert ffi.sizeof("planck_report") == 64
    assert ffi.sizeof("planck_market_event") == 40

    symbols = ffi.new("planck_symbol[]", [{"id": 5, "band_low": -512, "band_ticks": 1024}])
    config = ffi.new("planck_config*")
    lib.planck_config_init(config)
    assert config.struct_size == ffi.sizeof("planck_config")
    config.symbols = symbols
    config.symbol_count = 1
    config.order_capacity = 128
    config.huge_pages = lib.PLANCK_HUGE_PAGES_OFF

    handle = ffi.new("planck_engine**")
    assert lib.planck_engine_create(config, handle) == lib.PLANCK_OK
    engine = ffi.gc(handle[0], lib.planck_engine_destroy)

    def submit(session, side, price, qty, client_order_id, tif=lib.PLANCK_DAY):
        order = ffi.new("planck_order*", {
            "client_order_id": client_order_id, "session": session, "symbol": 5,
            "side": side, "type": lib.PLANCK_LIMIT, "price": price, "qty": qty, "tif": tif})
        return lib.planck_submit(engine, order)

    # Three bids at negative prices, best first in time at -10.
    assert submit(1, lib.PLANCK_BUY, -10, 5, 100) == lib.PLANCK_OK
    assert submit(2, lib.PLANCK_BUY, -10, 7, 200) == lib.PLANCK_OK
    assert submit(3, lib.PLANCK_BUY, -11, 9, 300) == lib.PLANCK_OK
    accepted = reports(ffi, lib, engine)
    assert [r["kind"] for r in accepted] == [lib.PLANCK_REPORT_ACCEPTED] * 3
    assert [r["client_order_id"] for r in accepted] == [100, 200, 300]
    assert len({r["order_id"] for r in accepted}) == 3

    quote = ffi.new("planck_quote*")
    assert lib.planck_best(engine, 5, lib.PLANCK_BUY, quote) == lib.PLANCK_OK
    assert (quote.valid, quote.price, quote.qty) == (1, -10, 12)

    # A sell of 14 at -11 takes 5, then 7, then 2 of the 9: price, then time.
    assert submit(4, lib.PLANCK_SELL, -11, 14, 400, lib.PLANCK_IOC) == lib.PLANCK_OK
    out = reports(ffi, lib, engine)
    assert out[0]["kind"] == lib.PLANCK_REPORT_ACCEPTED and out[0]["session"] == 4
    fills = [r for r in out if r["kind"] == lib.PLANCK_REPORT_EXECUTED]
    makers = [r for r in fills if r["liquidity"] == lib.PLANCK_MAKER]
    takers = [r for r in fills if r["liquidity"] == lib.PLANCK_TAKER]
    assert [(r["session"], r["qty"], r["price"], r["leaves"]) for r in makers] == \
        [(1, 5, -10, 0), (2, 7, -10, 0), (3, 2, -11, 7)]
    assert [r["leaves"] for r in takers] == [9, 2, 0]
    assert len(out) == 7, "an order filled in full must not also be cancelled"

    assert lib.planck_best(engine, 5, lib.PLANCK_BUY, quote) == lib.PLANCK_OK
    assert (quote.valid, quote.price, quote.qty) == (1, -11, 7)

    # The public stream: three adds and three trades.
    event = ffi.new("planck_market_event*")
    kinds = []
    while lib.planck_next_market_event(engine, event) == 1:
        kinds.append(event.kind)
    assert kinds == [lib.PLANCK_MARKET_ADD] * 3 + [lib.PLANCK_MARKET_TRADE] * 3

    # The market's refusals come as reports. The caller's mistakes as statuses.
    assert submit(1, lib.PLANCK_BUY, 100000, 1, 500) == lib.PLANCK_OK
    refused = reports(ffi, lib, engine)
    assert refused[0]["kind"] == lib.PLANCK_REPORT_REJECTED
    assert refused[0]["reason"] == lib.PLANCK_REJECT_PRICE_OUT_OF_BAND
    assert ffi.string(lib.planck_reject_name(refused[0]["reason"])) == b"PriceOutOfBand"
    assert submit(0, lib.PLANCK_BUY, -10, 1, 600) == lib.PLANCK_INVALID_ARGUMENT
    assert lib.planck_submit(engine, ffi.NULL) == lib.PLANCK_INVALID_ARGUMENT

    stats = ffi.new("planck_stats*")
    assert lib.planck_get_stats(engine, stats) == lib.PLANCK_OK
    assert (stats.commands, stats.trades, stats.rejected, stats.resting) == (5, 3, 1, 1)

    print("PASS")


if __name__ == "__main__":
    main()
