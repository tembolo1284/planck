# Building

Everything goes through `./build.sh`, which is a front end to the presets in
`CMakePresets.json`. You can also use the presets directly, or plain CMake.
All three end up in the same place.

## build.sh

```
./build.sh [command] [options] [-- extra arguments]
```

| Command | Does |
|---------|------|
| `build` | Configure if needed, then build. The default. |
| `test` | Build, then run the tests. |
| `rebuild` | Remove this configuration's directory and build again. |
| `clean` | Remove this configuration's directory. `--all` removes `build/`. |
| `bench` | Build, then run `planck-bench`. |
| `list` | Show the ten configurations and which have been built. |
| `help` | The full menu. |

| Option | Meaning |
|--------|---------|
| `--gcc`, `--clang` | Which compiler. GCC is the default. |
| `--cxx PATH`, `--cc PATH` | An exact compiler executable, for example `g++-16`. A build directory that was made with another compiler is started afresh, and stays on the new one afterwards. |
| `--release`, `--debug`, `--checked`, `--asan`, `--tsan` | The build kind. See below. |
| `-j N` | Parallel jobs. Default is every CPU. |
| `--target NAME` | Build one target, for example `planckd`. |
| `-R REGEX`, `-L LABEL` | `test`: run a subset. |
| `--portable` | Turn off `-march=native`. |
| `--fresh` | Reconfigure from nothing. |
| `-v` | Show compiler commands and all test output. |

Anything after `--` goes to ctest, to `planck-bench`, or to Ninja.

## The ten configurations

Two compilers times five kinds. Each gets its own directory,
`build/<compiler>-<kind>/`, so switching never forces a rebuild.

| Kind | Optimisation | Contracts | Use it for |
|------|--------------|-----------|------------|
| `release` | `-O3` | assumed | Measuring and running. The default. |
| `debug` | `-O0 -g` | checked | Stepping through in a debugger. |
| `checked` | `-O2 -g` | checked | Running the tests. |
| `asan` | `-O2 -g` | checked | Memory errors and undefined behaviour. |
| `tsan` | `-O2 -g` | checked | Data races. |

**Contracts** are `PLANCK_EXPECT`, `PLANCK_ENSURE` and `PLANCK_ASSERT`
(see `include/planck/core/contract.hpp`). "Checked" tests each one at run
time and aborts with file and line if it fails. "Assumed" hands the
condition to the optimiser as a fact and emits no code. So a release build
is faster, and a broken contract in it is undefined behaviour. Run tests in
`checked`. Run them in `release` as well, because the optimiser acts on the
assumptions and that deserves testing too.

`scripts/check_all.sh` builds and tests six of the ten in one go (both
compilers in release and checked, plus GCC with each sanitizer). `--full`
does all ten, `--quick` does two.

## Presets and plain CMake

```bash
cmake --preset gcc-checked
cmake --build --preset gcc-checked
ctest --preset gcc-checked
```

or with no presets at all:

```bash
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out
ctest --test-dir out -j --output-on-failure
```

## CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `CMAKE_BUILD_TYPE` | `Release` | The usual. |
| `PLANCK_CONTRACTS` | `auto` | `auto`, `off`, `assume` or `checked`. Auto is checked without `NDEBUG` and assume with it. |
| `PLANCK_SANITIZE` | empty | Passed to `-fsanitize=`, for example `address,undefined` or `thread`. |
| `PLANCK_NATIVE` | `ON` | `-march=native`. Turn off for a binary that must run elsewhere. |
| `PLANCK_BUILD_TESTS` | `ON` | The test suite. Fetches GoogleTest if it is not installed. |
| `PLANCK_BUILD_APPS` | `ON` | `planckd` and the three tools. |
| `PLANCK_BUILD_CAPI` | `ON` | `libplanck.so`. |
| `PLANCK_BUILD_BENCH` | `ON` | The benchmarks. Needs the apps. |

Compiler flags for every target: `-Wall -Wextra -Wpedantic -Werror
-Wconversion -Wsign-conversion -Wshadow -fno-exceptions -fno-rtti`.

## Targets

Libraries, each a static library with an alias `planck::<name>`. An arrow
means "links".

```
core <- sys <- mem <- book  <-+
                 ^            |
                 +--- ring <--+-- engine <-+-- gateway (also net, perf)
core <- wire <----------------+            +-- feed    (also net)
sys  <- net                                +-- journal
core <- perf
```

| Target | Kind | Where it lands |
|--------|------|----------------|
| `planck_core` ... `planck_journal` | static libraries | `build/<cfg>/` |
| `planckd`, `planck-client`, `planck-mdtap`, `planck-replay` | programs | `build/<cfg>/apps/` |
| `planck-bench`, `planck-loadgen` | programs | `build/<cfg>/bench/` |
| `planck_capi` | shared library `libplanck.so` | `build/<cfg>/capi/` |
| `planck_unit_tests`, `planck_property_tests`, `planck_integration_tests`, `planck_apps_tests`, `planck_bench_tests`, `planck_capi_tests`, `planck_capi_smoke` | test programs | `build/<cfg>/tests/`, `capi/` |

Two things about `libplanck.so` are unusual and deliberate (S-17, S-18):

- It compiles the engine's sources a second time, as position independent
  code. The static libraries the venue links are untouched, so the venue's
  hot path does not pay for a library it does not use.
- It is linked as a C library, with a version script that exports only
  `planck_*`. It needs nothing but libc at run time. The `capi_exports`
  test checks both.

## Adding a source file

1. Header in `include/planck/<module>/`, source in `src/<module>/`.
2. Add the `.cpp` to that module's `add_library` in the root
   `CMakeLists.txt`.
3. Add a test file to the right list in `tests/CMakeLists.txt`.

For a new module, add an `add_library`, an alias and its
`target_link_libraries` in dependency order, in the same file. If the C
library should include it, add it to the `foreach` in `capi/CMakeLists.txt`.

## Troubleshooting

| Problem | What to do |
|---------|------------|
| `build/ holds an old build made without presets` | `rm -rf build`, once. |
| A warning breaks the build on a new compiler | That is `-Werror` doing its job. Fix the code. If the warning is wrong, suppress it at the one site with a comment saying why. |
| `contract mode 'assume' needs [[assume]]` | The compiler is neither GCC 13+ nor Clang. The usual cause is that plain `g++` is an old one and the new one has another name: `g++ --version` tells. Name the new one with `--cxx g++-16 --cc gcc-16`. GCC 11 does build the checked and sanitizer kinds, but it is not a compiler this is developed with. |
| A sanitizer build runs as many tests as an ordinary one, or as fast | It is not a sanitizer build. `scripts/check_all.sh` prints the compiler of each configuration, and a sanitizer configuration runs one test fewer than the others. An old `build.sh` could leave a directory this way (F-32). The present one notices and starts the directory afresh. |
| Clang sanitizer builds fail to link | Install Clang's runtime (`libclang-rt-dev`). Not tested in development (W-77). |
| Link errors about `-fPIC` in `capi/` | Something was added to the venue's libraries that the C library also needs. See "Adding a source file". |
