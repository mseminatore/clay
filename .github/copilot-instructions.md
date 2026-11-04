# Copilot Instructions — Clay Language

## Build & Test

Clay uses CMake (primary) with a Unix Makefile fallback. C99 standard.

```sh
# Configure and build (Windows — auto-detects Visual Studio)
cmake -S . -B build
cmake --build build

# Build (Unix)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# Run all runtime tests
ctest --test-dir build -C Debug --rerun-failed --output-on-failure

# Or run the test executable directly
build\Debug\test_runner.exe    # Windows
./build/test_runner             # Unix
```

Individual tests cannot be run in isolation — `test_main()` in `test/test_runtime.c` runs all suites sequentially. To test a specific area, temporarily comment out other `test_*()` calls in `test_main`.

There is no linter configured.

## Architecture

Clay is a prototype-based OO language with two execution paths from a shared frontend:

```
.clay source → Lexer → Parser → AST ─┬→ C CodeGen  → .c → system C compiler → binary
                                      └→ BC Compiler → bytecode → VM → result
```

### Three layers

1. **Compiler frontend** (`src/compiler/`): Lexer, parser, and AST — shared by both backends. `codegen.c` emits human-readable C with `#line` directives. `bc_compiler.c` compiles AST to bytecode for the VM.

2. **Runtime** (`src/runtime/`): The object system used by both backends. `clay.h` is the central header. Core prototypes (`clay_Object`, `clay_Number`, `clay_String`, etc.) are global singletons initialized in `bootstrap.c`. Each type module (`string.c`, `number.c`, `array.c`, etc.) registers methods on its prototype via `clay_*_init_methods()`.

3. **VM** (`src/vm/generated/`): Bytecode VM generated from `src/vm/clay.yaml` by vmgen (`deps/vmgen` submodule). Files in `src/vm/generated/` are marked "DO NOT EDIT" — modify the YAML spec and regenerate with `cmake --build build --target regen_vm`.

### Key runtime concepts

- **Tagged values**: `ClayValue` is a `uintptr_t`. Low bit = 1 means small integer (value >> 1). Otherwise it's a `ClayObject*` pointer. `CLAY_NIL` is 0. Booleans are global singletons (`clay_true_obj`, `clay_false_obj`).
- **Message dispatch** (`dispatch.c`): All operations go through `clay_send()` / `clay_send_val()`. For tagged integers, a temporary `clay_Number` wrapper is created. Methods are looked up walking the prototype chain, then slots are checked as a fallback.
- **Memory**: Reference counting via `clay_release()` / `clay_retain()`. The hash table library (`deps/hash/`) never frees keys or values — callers own memory.
- **Prototype chain**: `ClayObject.parent` links form the chain. Slot reads and method lookups walk parents. Slot writes are always local to the object.

## Conventions

- **Method signatures**: All runtime methods follow `ClayValue fn(ClayObject *self, int argc, ClayValue *argv)`. Unused parameters are cast to `(void)`.
- **Error handling**: `clay_raise()` uses `setjmp`/`longjmp` via `clay_try_push()`/`clay_try_pop()`. Error types: `DivisionByZero`, `MethodNotFound`, `NilError`, `TypeError`, `RuntimeError`.
- **Test framework**: Uses `testy` (`deps/testy/`). Tests use `MODULE()`, `SUITE()`, `COMMENT()`, `TEST()`, and `TESTEX()` macros. New test suites go as `static void test_*()` functions called from `test_main()` in `test/test_runtime.c`.
- **Naming**: Runtime API is `clay_*` (e.g., `clay_clone`, `clay_send`, `clay_string_new`). VM API is `clayvm_*`. Type constructors follow `clay_<type>_new()`.
- **MSVC compatibility**: `_CRT_SECURE_NO_WARNINGS` is defined. `strdup` is aliased to `_strdup` under `_MSC_VER`. Windows console API is declared manually in `main.c` to avoid `windows.h` name conflicts.
- **Dependencies** (`deps/`): `hash` (hash table for slots/methods) and `testy` (test framework) are git submodules. The hash library has its own `deps/hash/.github/copilot-instructions.md` with detailed API docs.
