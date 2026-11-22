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
- Builds are optimized by default (`RelWithDebInfo` when no build type is given). Outside Debug, the
  `clay` binary links LTO-built copies of the runtime libraries (`<lib>_lto` targets, made from the
  originals' sources and settings); the plain libraries stay as they are for `clay --compile` and the
  tests. `-DCLAY_LTO=OFF` turns it off.

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
   library so native programs can run bytecode too. `check.c` reports undefined variables and a
   `break`/`continue` with no loop before either backend runs (mirrored by `GlobalCheck` in `selfhost/clay-parser`; `--no-check` skips it).
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
  (`clay_cached_lookup`, which also caches "no method": slot reads sent as messages) plus inline
  caches in the BC compiler. Every cache entry records `clay_dispatch_epoch`, which goes up when a
  method is defined anywhere, a selector becomes shadowed by a slot, or an object's methods table is
  freed (so a table later made at its address can't match); an entry from an older epoch
  is never used, so a new kind of method cache must check it too.
- **Memory**: reference counting via `clay_retain()`/`clay_release()` (atomic when `CLAY_THREAD_SAFE`
  is defined). The `deps/hash` hash table never frees keys/values — callers own that memory. Clay
  builds it with its own settings (`src/runtime/clay_hash_config.h`: no stats, 4 embedded entries,
  load factor 3/4), which change the `HashTable` layout: `clay.h` includes that header before `hash.h`,
  every build compiles `src/runtime/clay_hash.c` (hash.c with the settings) instead of `hash.c`, and
  `clay_bootstrap` exits if the library's layout differs. `ClayCell`
  is a heap-allocated box used for by-reference variable capture in C codegen.
- **Cycle collector** (end of `memory.c`): synchronous trial deletion (Bacon & Rajan). `clay_release`
  buffers objects/cells whose count drops without reaching zero; `clay_gc_collect` runs at the safe
  point in `CLAY_STACK_CHECK()` (method/block entry) or on `System collect`. It never scans stacks or
  globals, so it is only safe if **every holder of an object holds a count** — never keep an uncounted
  pointer to an object across a call into Clay code. `gc_children` must follow exactly the references
  `clay_dealloc` releases (a new kind of counted reference held by an object must be added to both,
  and to `gc_clear`); references it doesn't follow just keep their targets alive. Test changes with an
  ASan build and `CLAY_GC_STRESS=1` (collects at every safe point).
- **Prototype chain**: `ClayObject.parent` links form the chain used by both slot reads and method
  lookup. Slot *writes* are always local to the receiving object (no chain walk).
- **Error handling**: `clay_raise()`/`clay_try_push()`/`clay_try_pop()` implement try/catch via
  `setjmp`/`longjmp` over a linked list of frames on the C stack (no fixed nesting limit). An error
  nothing catches is reported by `clay_error_traceback` (error.c: each line a place in the code and the
  method/block it is in, from the frame kinds `clay_trace_enter` marks; repeats counted, long traces cut in
  the middle) from `main.c`, the native no-handler path and clayc's
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
  values because runtime callers (`each`, `map`, `ifTrue`) release them. Everything a generated
  function owns is also registered on the unwind list (`clay_uw_temp`/`clay_uw_var`/`clay_uw_cell`,
  inline in clay.h), so an error, break, continue or non-local return unwinding past the frame
  releases it; cleanup drops the entries (`clay_uw_drop`, `clay_uw_reset(__uw)` before the exit
  releases). Code that moves a temp's reference anywhere but at statement cleanup must set the temp
  to nil (see `emit_raise`), or the unwinder releases it twice. Debug with an ASan build
  configured with `-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON` (SDL2 + ASan hangs on macOS).
- **Fibers** live in `src/runtime/fiber.c` (C-stack switching); the VM swaps its own stacks via
  `clay_fiber_hooks` registered in `vm_bridge.c`.
- **File scope of block assignments**: top-level variables are globals shared by every file (imports
  are spliced into one program; each `AstNode` records its `file`), but a top-level block's `x = v`
  writes a global only when `x` is assigned at the top level of the block's own file; otherwise `x` is
  the block's local. `bc_is_file_global` (bc_compiler), `emit_shadow_decls` (codegen) and their
  selfhost ports (`isFileGlobal`, `shadowDecls`) implement it — keep them in sync.
- **Conditional jumps**: the VM's `JUMP_IF_FALSE` tests Clay truthiness itself (`nil`, `false`, `0`,
  `+0.0`, as `CLAY_IS_TRUTHY`), so both compilers drop a `to_bool` / `is_truthy` call emitted just before
  one (`bc_drop_truth_tests`, called from `emit_jump`; selfhost `dropTests`), unless a jump lands after
  its start: `patch_jump` records the last jump target, `emit_op` where the last instructions start,
  and both are saved around each chunk. Keep the two in sync.
- **Inlined ifTrue/ifFalse**: in a method or block body, `c ifTrue { ... }` / `c ifFalse { ... }` with a
  parameterless block literal (`ast_is_inline_if`) runs the block's statements in place
  (`bc_emit_inline_if`; selfhost `inlineIf`), and two in a row are one test. The block's own variables
  become locals of an inner scope, reset each time it runs; its `return` returns from the method, or
  outside methods ends just the block. The runtime side (`clay_cond_test`/`clay_cond_ran`/
  `clay_cond_skipped` in bootstrap.c) gives the methods' results. Capture prescans and the non-local
  return check (`ast_has_block_return_inlined`) treat such blocks as part of the enclosing code.
  Codegen does the same in C (`emit_inline_if`; selfhost `inlineIf`): the block's variables are C
  locals of the branch, released when it ends.
- **Inlined whileTrue/whileFalse**: `{ c } whileTrue { ... }` with parameterless block literals runs in
  place too, when every `break`/`continue` that targets it can be a jump (`ast_is_inline_while`: none in
  a try block, a receiver block or an expression; a block passed to a message keeps its own); otherwise
  it is sent. `bc_emit_inline_while` / `emit_inline_while`
  (selfhost `inlineWhile` in both ports).
- **Inlined counting loops**: `n times { |i| ... }` and `(a..b) each { |i| ... }` (block literal with at
  most one plain parameter, same jump condition: `ast_inline_count_kind`) run in place too:
  `bc_emit_inline_count` / `emit_inline_count` (selfhost `inlineCount`), bounds from `clay_times_count` /
  `clay_range_bound` (VM natives `times_count` / `range_bound`), tested before the first run and after each.
  Values match the messages (times: last body value, nil after break/continue; each: the range, nil after
  break). The bc compiler gives the parameter a scope (and `bc_slot_flag_local` ignores an outer `?x` flag);
  codegen has no inner scopes, so its prewalk renames the parameter in the body (`rename_var`, `__p<n>_i`).
- **break / continue**: lexical, as in Ruby. `check.c` `check_loop_control` (selfhost `GlobalCheck _loop`)
  rejects one outside a loop body or a block literal passed to a message (ifTrue/ifFalse and try blocks and
  a send's receiver block are transparent). Each belongs to the innermost inlined loop (a jump) or block
  literal passed to a message other than ifTrue/ifFalse or a whileTrue/whileFalse body
  (`ast_block_is_loop_arg`). A continue ends that run of the block with nil (`stop_vm` / function exit).
  A break ends the call: such a send (`ast_send_catches_break`) is `send_break` in the VM /
  `clay_send_owned_catch_break` in C, which gets a `clay_break_id_next()` id, tags its block args
  (`ClayBlock.break_id`) and catches only the break carrying that id; the block raises it with
  `clay_raise_break` (the VM trampoline converts a plain one; codegen raises it directly). Signals raised in
  a sub-block (try, `{ } value`) are caught and converted by the owning block (VM trampoline, codegen
  `__sig_tf` when `ast_block_sub_raises`). Loop natives (each, times, ...) catch only plain (untagged)
  breaks, which now come only from whileTrue/whileFalse bodies sent as messages. Chunks record
  `loop_owner` (unit field 8; `--bytecode` prints it). Selfhost: `isLoopArg`, `sendCatchesBreak`,
  `blockSubRaises`, `ownedCtl` in both ports.
- **Implicit-slot fallback**: a bare `x = v` in a method writes self's slot `x` if it exists at call
  time; both compilers implement this with a hidden per-local flag (`?x` in bc_compiler, `__isslot_x` in
  codegen) set at method/block entry — keep the two in sync. In the VM each read/write of such a local
  is one `GET_LOCAL_FLAGGED`/`SET_LOCAL_FLAGGED` (slot, flag, name constant); the flag-set and
  captured-cell cases run in `vm_bridge.c` (`vm_flagged_get`/`vm_flagged_set`, the VM's
  `flagged_get`/`flagged_set` hooks).
- **Source locations**: runtime errors, tracebacks and the debugger use `clay_source_line` *and*
  `clay_source_file`, set from each AST node's `file` (imports are spliced into one program). The
  VM keeps a per-offset `file_map` for top-level code (which mixes files) and one `BcChunk.file`
  per body; codegen writes `clay_source_file = clay_files[k];` per statement when a program has
  several files; `VM run` units carry `[offset, path]` file marks. The selfhost ports (`setLoc`,
  `fileMarks`, `emitLine`) must match — `--bytecode` prints the files, so parity checks cover it.
  Files under the bundled `lib/` are *shown* relative to it (`clay_display_file`; the lib dir comes
  from `resolve_lib_dir`, a compiled program's `clay_set_lib_dir` call, or clayc's `VM setLibDir`).
- **Call syntax `foo(args)`**: parsed as `Send(Ident foo, "value", args)` with `is_call` set
  (`Call(foo)` in `--ast`). A compiler sends `self foo(args)` instead when `foo` would resolve to self
  like a bare name (`bc_name_is_self_slot` / `call_targets_self`; selfhost `selfSlotName` /
  `callTargetsSelf`). Bare reads and calls treat methods declared in the clone body like declared
  slots (`known_member_count` after the slot names); assignments still use only the slots.

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
