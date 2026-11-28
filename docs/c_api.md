# The C interface

`libplanck.so` is planck's matching engine behind a plain C interface, so
that C, Python, Rust, Go, Java or anything else with a foreign function
interface can use it. It is the engine only: one shard, driven on the
caller's thread. No sockets, no threads, no journal (W-90, D-15).

Good for: a simulator, a backtest, a test harness for a trading strategy.
Not for: running or talking to a venue.

| File | What |
|------|------|
| `capi/include/planck.h` | The whole interface. Every function is documented there. |
| `build/<cfg>/capi/libplanck.so` | The library |
| `capi/tests/capi_smoke.c` | A complete C program using it |
| `capi/tests/capi_cffi_test.py` | The same from Python |

## The shape of it

This is the "hourglass" pattern: a wide C++ implementation, a narrow C
waist, and whatever the caller's language wants on top. planck has the
bottom and the waist. The C++ wrapper that would form the top is deferred
(D-17).

Rules the interface keeps, and that any change to it must keep:

- **One opaque handle**, `planck_engine*`. Everything else is a fixed-layout
  struct of fixed-width integers with no padding surprises.
- **Nothing but `planck_*` is exported**, and the library needs only libc
  at run time (S-18). The `capi_exports` test checks both.
- **No callbacks.** The caller pulls results (D-16).
- **No call aborts on bad input.** A caller's mistake comes back as a
  status and changes nothing. A market's refusal comes back as a report
  (S-19).
- **Versioned.** `planck_abi_version()` must equal `PLANCK_ABI_VERSION`.
  `planck_config` carries its own size, so it can grow.

## A minimal program

```c
#include <planck.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    if (planck_abi_version() != PLANCK_ABI_VERSION) return 1;

    const planck_symbol symbol = {.id = 1, .band_low = 1000, .band_ticks = 1024};
    planck_config config;
    planck_config_init(&config);          /* always start from this */
    config.symbols = &symbol;
    config.symbol_count = 1;

    planck_engine* engine = NULL;
    if (planck_engine_create(&config, &engine) != PLANCK_OK) return 1;

    planck_order order;
    memset(&order, 0, sizeof order);      /* reserved bytes must be zero */
    order.client_order_id = 41;
    order.session = 7;
    order.symbol = 1;
    order.side = PLANCK_SELL;
    order.type = PLANCK_LIMIT;
    order.price = 1500;
    order.qty = 10;
    order.tif = PLANCK_DAY;
    if (planck_submit(engine, &order) != PLANCK_OK) return 1;

    planck_report report;
    while (planck_next_report(engine, &report) == 1) {
        printf("report kind=%u order_id=%llu\n", report.kind,
               (unsigned long long)report.order_id);
    }
    planck_market_event event;
    while (planck_next_market_event(engine, &event) == 1) {
        printf("market kind=%u price=%d qty=%u\n", event.kind, event.price, event.qty);
    }

    planck_engine_destroy(engine);
    return 0;
}
```

```bash
cc -std=c11 demo.c -Icapi/include -Lbuild/gcc-release/capi -lplanck -o demo
LD_LIBRARY_PATH=build/gcc-release/capi ./demo
```

## Functions

| Function | Does |
|----------|------|
| `planck_abi_version` | The library's ABI version. Compare with `PLANCK_ABI_VERSION`. |
| `planck_version` | Version string |
| `planck_status_name`, `planck_reject_name` | Names for codes. Never null. |
| `planck_config_init` | Fills a config with defaults and its size |
| `planck_engine_create` | Builds an engine. Takes about 50 ms, to calibrate its clock (W-93). |
| `planck_engine_destroy` | Frees it. Null is fine. |
| `planck_submit` | One new order |
| `planck_cancel` | Cancel by engine order id. Only the owning session may. |
| `planck_next_report` | Next private report: 1 if one was written, 0 if none |
| `planck_next_market_event` | Next public market event, same convention |
| `planck_best` | Best price and total quantity on one side |
| `planck_quantity_at` | Total quantity resting at one price |
| `planck_get_stats` | Counters |

## The model

**Submit, then read.** `planck_submit` and `planck_cancel` return whether
the engine *took* the command, not what the market did with it. What
happened arrives as reports (private, addressed to a session) and market
events (public, anonymous), exactly the two streams a venue produces. Read
both queues empty after each call.

**Status against report.** Statuses are for the calling program's bugs:
null pointer, a side that is neither buy nor sell, non-zero reserved bytes.
Reports are for the market's answers: unknown symbol, price outside the
band, book full, fill-or-kill not fillable. A trading program has to handle
the second kind anyway.

**Sessions** are just numbers the caller picks, 0 to 65,535. They say who
owns an order. A cancel from another session is refused with a report.

**`PLANCK_WOULD_BLOCK`** means there is too much unread output. Nothing was
done. Read the queues and call again. The engine holds back whenever the
unread output leaves less room than the worst case one order could need,
since on a single thread nothing can drain the queues in the middle of a
match.

**Threads.** One engine, one thread at a time. No locking inside. Separate
engines are independent.

**Time.** Timestamps come from the engine's own clock. The caller cannot
supply them, so two runs give the same books and different timestamps
(W-92).

## Memory

The output queues are sized for the worst case: one order sweeping a full
book. That is about 400 bytes per order of capacity, so about 25 MB at the
default `order_capacity` of 65,536 and about 870 MB at the limit of
4,000,000 (W-91). Set `order_capacity` to what you need.

## From Python

`capi/tests/capi_cffi_test.py` is the worked example. The header is written
so that cffi can read it directly: define `PLANCK_CFFI` and it drops its
includes and attributes.

```python
import cffi, subprocess
ffi = cffi.FFI()
header = subprocess.run(["cc", "-E", "-P", "-DPLANCK_CFFI", "capi/include/planck.h"],
                        capture_output=True, text=True, check=True).stdout
ffi.cdef(header)
lib = ffi.dlopen("build/gcc-release/capi/libplanck.so")
assert lib.planck_abi_version() == lib.PLANCK_ABI_VERSION
```

This is what the test does.

## How it is built

- The engine's sources are compiled a second time as position independent
  code, into the shared library only (S-17).
- `capi/planck.map` is a linker version script that exports `planck_*` and
  hides everything else.
- The library is linked as C, with `--as-needed`, so it does not pull in the
  C++ runtime (F-13). The implementation allocates with `aligned_alloc` and
  placement new for that reason. Do not add anything that needs libstdc++
  at run time: no `std::string`, no `new`, no exceptions, no iostreams.

## Tests

| Test | Checks |
|------|--------|
| The suites in `tests/unit/capi_test.cpp` | Every function, every bad argument, the hold-back rule at its edges, and a random run compared against a shard driven directly |
| `capi_smoke` | A C11 program, compiled as C, links and runs |
| `capi_exports` | Only `planck_*` is exported, and only libc is needed |
| `capi_cffi` | The same engine from Python. Skipped without cffi and in sanitizer builds. |

## Changing the interface

1. Adding a function: add it to `planck.h` and `planck.cpp`, with a name
   starting `planck_` so the version script exports it. No version bump.
2. Adding a field to `planck_config`: add it at the end. `struct_size` lets
   old callers keep working. No version bump.
3. Changing any other struct, any enum value or any function's meaning:
   bump `PLANCK_ABI_VERSION`.
4. Add the bad-argument tests and a seeded bug for every new check at the
   boundary. The promise that no call aborts rests on those checks being
   complete (W-95).
