# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Clay is a prototype-based, pure object-oriented language ("everything is an object", message-passing,
clone-to-create) written in C99. It has two execution paths from a shared frontend: compile to
human-readable C (native binaries) or compile to bytecode for an embedded VM. See `README.md` for
full language documentation and `docs/prerelease-review.md` for the open bug/feature tracker.

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
- `.clay` integration programs live in `test/programs/` and are each registered individually in
  `CMakeLists.txt` as `add_test(NAME clay_test_<name> COMMAND clay_compiler <path>.clay)` — running the
  program with a zero exit code is the test.
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
   compiles the AST directly to bytecode for the VM. `bricks.c` implements the package manager.
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
   (or `cmake -P cmake/regen_vm.cmake`; add `-- --check` to only verify). Known exception:
   `vm.h`/`clayvm.h` carry a hand-added `CLAYVM_VAL_CELL` that vmgen cannot express yet, so
   regenerating those two files currently breaks the build — see `docs/prerelease-review.md` §1.4.

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
  `setjmp`/`longjmp` over a linked list of frames on the C stack (no fixed nesting limit). Built-in
  error types: `DivisionByZero`, `MethodNotFound`, `NilError`, `TypeError`, `RuntimeError`.

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

## Bricks (package manager)

Clay has a built-in package manager: packages ("bricks") are plain Git repos with a `brick.json`
manifest, resolved via the `bricks/` directory (gitignored, recreated by `clay bricks install`).
Monorepo bricks (e.g. `clay-stdlib`) are imported with slash paths like
`import("clay-stdlib/clay-collections")`. See the "Bricks" section of `README.md` for the full
manifest format and CLI (`clay bricks init|add|install|update|list|remove`).

## Editor integration

`editors/vscode/clay-lang/` is a VS Code extension providing syntax highlighting and a DAP client
that talks to `clay --dap`.
