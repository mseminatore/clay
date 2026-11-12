# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Clay is a prototype-based, pure object-oriented language ("everything is an object", message-passing,
clone-to-create) written in C99. It has two execution paths from a shared frontend: compile to
human-readable C (native binaries) or compile to bytecode for an embedded VM. The user guide lives
in `docs/` (start at `docs/README.md`); `README.md` is a short landing page.

## Build & Test

CMake is primary; a Makefile fallback exists for Unix.

```sh
# Configure & build
cmake -S . -B build
cmake --build build
# (Unix Makefile fallback: `make` / `make build`)

# Run all runtime/lexer/parser unit tests
ctest --test-dir build -C Debug --rerun-failed --output-on-failure
# or directly:
./build/test_runner            # Unix
build\Debug\test_runner.exe    # Windows

# Run the .clay integration programs (each is its own ctest entry, clay_test_*)
ctest --test-dir build -R clay_test_
# Makefile equivalent:
make test-clay
```

- Individual unit tests cannot be run in isolation — `test_main()` in `test/test_runtime.c` runs all
  suites sequentially (`MODULE`/`SUITE`/`TEST`/`TESTEX` macros from the `testy` framework). To focus on
  one area, temporarily comment out the other `test_*()` calls in `test_main`.
- Native backend: every test program is also compiled with `clay --compile` and run as
  `clay_native_<name>` (label `native`; `ctest -L native`). `--compile` reads `clay-native.cfg`, which
  CMake writes next to the binary (compiler, flags incl. `CMAKE_C_FLAGS`, prebuilt runtime libs), so
  an ASan build of clay produces ASan native binaries. Without the cfg (e.g. Makefile builds) it falls
  back to compiling the runtime sources listed in `clay_native_sources[]` in `main.c`.
- `.clay` integration programs live in `test/programs/` and are each registered individually in
  `CMakeLists.txt` as `add_test(NAME clay_test_<name> COMMAND clay_compiler <path>.clay)` — running the
  program with a zero exit code is the test, so a test program must exit non-zero on any failed
  check (`raise`, or `System exit(1)` after counting failures); printing "FAIL" is not enough.
  Demo-style programs without checks are golden-output tests instead (`clay_smoke_*`): their output
  must equal `test/programs/expected/<name>.out` (VM and native; `cmake/expect_output.cmake`).
- Docs tests: every ```` ```clay ```` block in `docs/*.md` and `README.md` is run by `doc_test`
  (`tools/doc_test.c`; CTest label `docs`, 30 s limit per block). Mark fragments and examples that
  block or need the network/a window/installed bricks as ```` ```clay norun ````.
- There is no linter configured.
- Optional CMake flag: `-DCLAY_THREAD_SAFE=ON` enables atomic refcounting + TLS (needed for
  `Thread`/`parallelMap` examples like `examples/raytrace_parallel.clay --live`, which also needs SDL2).
- OpenSSL is detected automatically for HTTPS support; without it, `Http` falls back to HTTP only.

## Running Clay programs

```sh
./build/clay program.clay          # run via bytecode VM (default, instant)
./build/clay --compile program.clay  # compile to native binary via generated C
./build/clay -c program.clay         # emit C only, don't compile
./build/clay --ast program.clay      # print AST and exit
./build/clay                         # VM REPL (default with no args)
```

Other useful flags: `--repl` (classic REPL, recompiles to C each input — slow but full-featured),
`--trace` (VM REPL with bytecode tracing), `--step` (interactive stepping debugger), `--dap` (Debug
Adapter Protocol server for the VS Code extension in `editors/vscode/clay-lang/`).

## Architecture

```
.clay source → Lexer → Parser → AST ─┬→ C CodeGen  → .c → system C compiler → binary
                                      └→ BC Compiler → bytecode → VM → result
```

### Three layers

1. **Compiler frontend** (`src/compiler/`): `lexer.c`/`parser.c`/`ast.c` are shared by both backends.
   `codegen.c` emits human-readable C with `#line` directives for debuggability. `bc_compiler.c`
   compiles the AST directly to bytecode for the VM; the runtime side of the VM (native functions,
   block/method trampolines, the `VM` prototype) is `src/runtime/vm_bridge.c`, part of the runtime
   library so native programs can run bytecode too. `check.c` reports undefined variables before
   either backend runs (mirrored by `GlobalCheck` in `selfhost/clay-parser`; `--no-check` skips it).
   `bricks.c` implements the package manager; `native_bricks.c` builds and loads native bricks
   (`brick.json` "native": C sources compiled into a plugin on first import, linked statically by
   `--compile`).
   `dap.c` implements the Debug Adapter Protocol server. `main.c` is the CLI driver (REPLs, flag
   parsing, plugin auto-load).

2. **Runtime** (`src/runtime/`): The object system shared by both execution paths. `clay.h` is the
   central header — read it first to understand `ClayValue`/`ClayObject`/dispatch. Core prototypes
   (`clay_Object`, `clay_Number`, `clay_String`, `clay_Array`, etc.) are global singletons created and
   wired together in `bootstrap.c`. Each type module (`string.c`, `number.c`, `array.c`, `dict.c`, ...)
   registers its methods on its prototype via a `clay_<type>_init_methods()` function called from
   `clay_bootstrap()`.

3. **VM** (`src/vm/generated/`): Bytecode VM **generated from `src/vm/clay.yaml`** by vmgen
   (`deps/vmgen` submodule). Files under `src/vm/generated/` are marked "DO NOT EDIT" — to change VM
   behavior, modify `clay.yaml` and regenerate with `cmake --build build --target regen_vm`
   (or `cmake -P cmake/regen_vm.cmake`; add `-- --check` to only verify — CI runs this). Clay-specific
   value types are declared in `clay.yaml`'s `types` list (e.g. `cell` → `CLAYVM_VAL_CELL` /
   `CLAYVM_CELL(v)`, used for by-reference captures).

### Key runtime concepts (in `clay.h`)

- **Tagged values**: `ClayValue` is a `uintptr_t`. 2-bit tag scheme — `& 3 == 0` is a pointer/nil,
  `== 1` is a small 62-bit integer (`value >> 2`), `== 3` is a tagged 32-bit float. `CLAY_NIL` is 0.
  Booleans are global singletons (`clay_true_obj`/`clay_false_obj`, accessed via `CLAY_TRUE`/`CLAY_FALSE`).
- **Message dispatch** (`dispatch.c`): everything goes through `clay_send_val()` (or the legacy
  `clay_send()` macro for object pointers). Tagged integers get a temporary `clay_Number` wrapper for
  dispatch. Method lookup walks the prototype chain (`ClayObject.parent`), with slots checked as a
  fallback (Io-style `doesNotUnderstand` semantics), and is accelerated by a global method cache
  (`clay_cached_lookup`) plus inline caches in the BC compiler.
- **Memory**: reference counting via `clay_retain()`/`clay_release()` (atomic when `CLAY_THREAD_SAFE`
  is defined). The `deps/hash` hash table never frees keys/values — callers own that memory. `ClayCell`
  is a heap-allocated box used for by-reference variable capture in C codegen.
- **Prototype chain**: `ClayObject.parent` links form the chain used by both slot reads and method
  lookup. Slot *writes* are always local to the receiving object (no chain walk).
- **Error handling**: `clay_raise()`/`clay_try_push()`/`clay_try_pop()` implement try/catch via
  `setjmp`/`longjmp` over a linked list of frames on the C stack (no fixed nesting limit). An error
  nothing catches is reported by `clay_error_traceback` (error.c: block-running calls dropped, repeats
  counted, long traces cut in the middle) from `main.c`, the native no-handler path and clayc's
  `report`; a fiber's uncaught error is raised again from `resume`. `CLAY_STACK_CHECK()` at the start
  of VM and generated methods/blocks turns C stack exhaustion into a `RuntimeError`. Built-in
  error types: `DivisionByZero`, `OverflowError`, `MethodNotFound`, `NilError`, `TypeError`, `RuntimeError`.
- **Integer range**: always build tagged ints with `clay_int()` (raises `OverflowError` outside 62 bits),
  multiply with `clay_int_mul()` and convert floats with `clay_float_to_int()`; never cast/shift by hand.
  Format floats with `clay_format_float()` (shortest round-trip text), integers with `%lld` + `(long long)`.
- **Generated-C ownership** (`codegen.c`): every C variable, cell and `__rv` owns one reference;
  stores emit retains *before* releasing old values; params are retained on entry; each function
  releases its variables and the cells it created before `return clay_return_value(...)`. Methods
  return results that alias self/args as borrowed (same as C natives); blocks always return owned
  values because runtime callers (`each`, `map`, `ifTrue`) release them. Debug with an ASan build
  configured with `-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON` (SDL2 + ASan hangs on macOS).
- **Fibers** live in `src/runtime/fiber.c` (C-stack switching); the VM swaps its own stacks via
  `clay_fiber_hooks` registered in `vm_bridge.c`.
- **File scope of block assignments**: top-level variables are globals shared by every file (imports
  are spliced into one program; each `AstNode` records its `file`), but a top-level block's `x = v`
  writes a global only when `x` is assigned at the top level of the block's own file; otherwise `x` is
  the block's local. `bc_is_file_global` (bc_compiler), `emit_shadow_decls` (codegen) and their
  selfhost ports (`isFileGlobal`, `shadowDecls`) implement it — keep them in sync.
- **Implicit-slot fallback**: a bare `x = v` in a method writes self's slot `x` if it exists at call
  time; both compilers implement this with a hidden per-local flag (`?x` in bc_compiler, `__isslot_x` in
  codegen) set at method/block entry — keep the two in sync.

## Conventions

- **Method signatures**: every runtime method is `ClayValue fn(ClayObject *self, int argc, ClayValue *argv)`;
  cast unused parameters to `(void)`. Dispatch pads `argv` to at least 4 slots with nil, but check
  argument types before dereferencing: use `CLAY_IS_OBJ(v)` / `CLAY_IS_TYPE(v, CLAY_TYPE_STRING)`
  (not `CLAY_IS_INT(v) || CLAY_IS_NIL(v)`, which misses tagged floats).
- **Return ownership**: a method returns a new reference, *except* that returning the receiver or one
  of its arguments (e.g. `return CLAY_VAL(self)`) is borrowed. Callers that need an owned result use
  `clay_send_owned()` (generated C does); the VM applies the same rule in `native_send_fn`.
- **Naming**: runtime API is `clay_*` (`clay_clone`, `clay_send`, `clay_string_new`, ...); VM API is
  `clayvm_*`; type constructors follow `clay_<type>_new()`.
- **MSVC compatibility**: `_CRT_SECURE_NO_WARNINGS` is defined; `strdup` aliases to `_strdup` under
  `_MSC_VER`; the Windows console API is hand-declared in `main.c` to avoid `windows.h` conflicts.
- **Dependencies** (`deps/`, some are git submodules — see `.gitmodules`): `hash` (slot/method hash
  table, has its own detailed `deps/hash/.github/copilot-instructions.md`), `testy` (test framework),
  `crossline` (REPL line editing), `cjson` (DAP/JSON), `sqlite` (the `Db` prototype), `vmgen` (VM
  generator, only needed to regenerate `src/vm/generated/`).

## Self-hosting (`selfhost/`)

A second compiler written in Clay: `clay-lexer`, `clay-parser`, `clay-bytecode` and `clay-codegen`
bricks plus the `clayc` driver. Each is a port of its C counterpart (`lexer.c`, `parser.c`,
`bc_compiler.c`, `codegen.c`) and must produce identical output — tokens, AST, bytecode and C —
checked by CTest (`ctest -L selfhost`: `clay_selfhost_<piece>[_native]`, `clay_selfhost_run`,
`clay_selfhost_bootstrap`). A change to one of those C files usually needs the same change in its
Clay port. `clay-bricks` is the port of `bricks.c` (`clayc bricks ...`), checked by
`clay_selfhost_bricks` (same output and files as `clay bricks`, against local git repos); a change
to `bricks.c` needs the same change there.

## Bricks (package manager)

Clay has a built-in package manager: packages ("bricks") are plain Git repos with a `brick.json`
manifest, resolved via the `bricks/` directory (gitignored, recreated by `clay bricks install`).
Monorepo bricks are imported with slash paths like `import("stdlib/collections")`; a dependency's
name in `brick.json` is its import path and its directory under `bricks/`. The standard library
(`lib/stdlib`, repo clay-stdlib) and the web bricks (`lib/web`, repo clay-web: http-server, router,
orm) are git submodules bundled with Clay: brick imports fall back to `lib/` (or `$CLAY_LIB`) after
every `bricks/` directory — `parser_set_lib_dir` in `parser.c`, set by `resolve_lib_dir` in
`main.c`, mirrored by `libDir` in `selfhost/clay-parser`. See `docs/bricks.md` for the full
manifest format and CLI (`clay bricks init|add|install|update|list|remove`).

## Editor integration

`editors/vscode/clay-lang/` is a VS Code extension providing syntax highlighting and a DAP client
that talks to `clay --dap`.
