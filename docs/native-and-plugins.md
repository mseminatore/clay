# Native compilation, FFI and plugins

Clay programs normally run in the bytecode VM. They can also be compiled to C
and built into native executables, and extended with C code through FFI or
plugins.

## Generated C

`clay --compile` turns a program into C and links it against the prebuilt
runtime libraries of the build that produced `clay` (recorded in
`clay-native.cfg` next to the binary), so compiling takes a fraction of a
second and the native program has the same features — HTTPS, Graphics,
plugins — as `clay` itself. Native programs are typically 3–4× faster than the
VM; use a Release build (`cmake -DCMAKE_BUILD_TYPE=Release`) so the runtime
they link against is optimized too.

The C file is kept: next to the source (`hello.clay` → `hello.c`), or next to
the binary when `-o` names one (`clay --compile -o build/hello hello.clay`
writes `build/hello.c`). `clay -c hello.clay` writes only the C.

Clay generates readable C. This Clay code:

```clay norun
p = Point clone
p moveTo(3, 4)
```

Generates:

```c
ClayValue p = clay_send_val(Point, "clone", 0, NULL);
clay_send_val(p, "moveTo", 2, (ClayValue[]){ clay_int(3), clay_int(4) });
```

The generated C includes `#line` directives mapping back to the `.clay` source, so debuggers (gdb, lldb, Visual Studio) step through Clay code.

## Architecture

```
.clay source → [Lexer] → [Parser] → [AST] ─┬→ [C CodeGen]  → .c → [C Compiler] → binary
                                            │                          ↑
                                            │                   Clay Runtime (linked)
                                            │
                                            └→ [BC CodeGen] → bytecode → [VM] → result
                                                                          ↑
                                                                   Clay Runtime (bridged)
```

**VM mode** (`clay file.clay`): compiles to bytecode and executes instantly in the embedded bytecode VM. The VM bridges to the same Clay runtime via native functions, so all core objects and methods work identically.

**C compile mode** (`clay -c file.clay`): generates human-readable C with `#line` directives, invokes the system C compiler, and links the Clay runtime as a static library.

**VM REPL mode** (`clay` or `clay --vm-repl`): interactive REPL that compiles expressions to bytecode and evaluates them instantly.

**DAP mode** (`clay --dap`): runs as a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/) server for VS Code and other DAP-compatible editors. Supports breakpoints, stepping, variable inspection, and output capture.

**Method cache**: message dispatch caches resolved methods per (receiver-prototype, selector) pair, avoiding repeated prototype-chain walks. The cache is automatically invalidated when methods are added or changed.

## A Clay compiler in Clay

`selfhost/` holds a Clay compiler written in Clay — lexer, parser, bytecode
compiler and C code generator as bricks, a port of the package manager, plus the `clayc` driver. Like
`clay`, `clayc` runs programs in the bytecode VM (which stays in C), has a
REPL, and builds native executables. Its bytecode and C are exactly what the
C compiler produces, and it compiles itself: built by `clay --compile`, then
by itself twice, it reaches a fixpoint (the CTest `clay_selfhost_bootstrap`).

```sh
clay --compile -o clayc selfhost/clayc.clay    # build the Clay compiler
./clayc examples/hello.clay                    # run a program in the VM
./clayc                                        # the REPL
./clayc -o hello examples/hello.clay           # a native executable
./clayc bricks list                            # the package manager, as clay bricks
```

Native programs reach the VM through the `VM` prototype: `VM run(unit)` loads
a program compiled by the clay-bytecode brick and runs it.

See [selfhost/README.md](../selfhost/README.md).

## FFI (Foreign Function Interface)

Load and call functions from shared libraries (`.dll`, `.so`, `.dylib`). Plugin functions must match the Clay method signature: `ClayValue fn(ClayObject *self, int argc, ClayValue *argv)`.

```clay norun
lib = FFI load("./my_plugin.dll")  // or .so / .dylib
result = lib call("my_function", arg1, arg2)
lib close
```

## Native plugins

Plugins are shared libraries that add new prototypes (or methods on existing
ones) written in C. They use only the stable API in
[`src/runtime/clay_plugin.h`](../src/runtime/clay_plugin.h) — no Clay internals —
and appear in Clay as ordinary objects:

```c
#include "clay_plugin.h"
#include <math.h>

static const ClayPluginAPI *api;

static ClayPluginValue mathex_sqrt(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("sqrt requires 1 argument", "ArgumentError"); return 0; }
    return api->make_float(sqrtf(api->to_float(argv[0])));
}

static ClayPluginMethod methods[] = { { "sqrt", mathex_sqrt }, { NULL, NULL } };

CLAY_PLUGIN_EXPORT void mathex_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "MathEx", methods, NULL, 0 };
    api->register_type(&def);
}
```

The entry point is named after the library, `<name>_plugin_init` (`mathex.so`:
`mathex_plugin_init`), so several plugins can be linked into one executable;
the older `clay_plugin_init` is still accepted for plugins loaded from a file.

```sh
cc -shared -fPIC -I src/runtime mathex.c -o plugins/mathex.so -lm   # Linux / macOS
cl /LD /I src\runtime mathex.c /Fe:plugins\mathex.dll                # Windows
```

```clay
MathEx = System plugin("mathex")   // loads plugins/mathex.{so,dylib,dll}
MathEx sqrt(16) println            // 4
```

`System plugin(name)` looks for `plugins/<name>` in the current directory, then
next to the running script, then next to the `clay` executable; it also accepts
a path. Every plugin in the `plugins/` folder beside the executable is loaded
automatically at startup, so its prototypes are available without the call
(loading the same plugin twice is harmless). The CMake build compiles the
examples in [`examples/plugins/`](../examples/plugins/) into `build/plugins/`.

### Plugin API version 2

`api->api_version` says which functions the table has; fields are only ever
added at the end. Version 2 adds what wrapping a C library needs:

- `retain` / `release`: keep a Clay value beyond the call (e.g. inside native
  data). `make_*`, `send`, `array_new` and `from_floats` return new references;
  arguments and `array_get` results are borrowed.
- Arrays: `is_array`, `array_new`, `array_size`, `array_get`, `array_push`.
- Numbers in bulk: `to_floats(arr, out, max)` flattens an Array (of rows) of
  numbers into a `float` buffer and returns the count (call it with `max` 0 to
  size the buffer); `from_floats(data, n, cols)` makes an Array of numbers, or of
  rows of `cols` numbers. Data crosses into C in one call, not value by value.

### Plugin API version 3

Version 3 adds `write_output(text)`: it writes where Clay's own output goes, which
is stdout, or the Debug Console when Clay runs as a debug adapter (`--dap`).
Use it for output the user should see (progress, logs) instead of `printf` or
`fputs(stdout)`, because in a debug session C's stdout is the protocol channel
and such text is lost. Check `api->api_version >= 3` before using it.

A C struct per object: register the type with a `free_fn`, `clone` the
prototype in a constructor method, `set_data` the struct on the clone, and
`get_data` it in the other methods; `free_fn` runs when the object is freed.

## Native bricks

`clay bricks new my-brick --native` makes a native brick with a working plugin, a
Clay wrapper and tests (see [Creating a brick](bricks.md#creating-a-brick)); read
`native/my_brick.c` for the shape. The rest of this section is the manifest it writes.

A brick can ship C sources: Clay compiles them into a plugin the first time
the brick is imported, with the same compiler and flags as `clay --compile`,
and keeps the result in the brick's `build/` directory (rebuilt when a source,
the compiler or the Clay version changes). A `clay --compile` executable links
the sources in directly, so it doesn't depend on the plugin file. The brick's
`brick.json` lists them:

```json
{
  "name": "ann",
  "main": "src/main.clay",
  "native": {
    "plugin": "ann",
    "sources": ["native/clay_ann.c", "native/libann/ann.c", "native/libann/tensor.c"],
    "cflags": ["-DUSE_FAST_MATH"],
    "libs": ["-lm", "-lpthread"]
  }
}
```

- `plugin` names the library, and so the entry point: `ann_plugin_init`.
- `sources` are relative to the brick, and may be in a git submodule:
  `clay bricks add` and `install` fetch submodules.
- Because a native brick runs C code it ships, `clay bricks add` says so when adding one and,
  at a terminal, asks before going ahead (`--yes` skips the question).
- `cflags` and `libs` are options for the compiler and linker. Each must start
  with `-` and contain only letters, digits and `. _ = + , / -`, because they end up
  on a command line. A `brick.json` that breaks these rules fails to import.

### Per-platform options and brick-relative paths

Each list can be extended for one platform with a key that adds `-macos`,
`-linux` or `-windows` (`linux` is any other Unix): `sources-macos`,
`cflags-linux`, `libs-windows`. Only the host's entries are used, appended to
the plain list, so a brick can use a platform's faster library where there is one:

```json
"native": {
  "plugin": "ann",
  "sources": ["native/clay_ann.c", "native/libann/tensor.c"],
  "libs": ["-lm"],
  "cflags-macos": ["-DUSE_BLAS", "-I@/native/shim"],
  "libs-macos": ["-Wl,-framework,Accelerate"]
}
```

- A flag has no spaces, so a linker option that takes an argument is written
  with commas: `-Wl,-framework,Accelerate`.
- `-I@/dir` and `-L@/dir` name a directory inside the brick (here
  `<brick>/native/shim`), whatever the current directory is. The `@/` is only
  accepted right after `-I` or `-L`.

### Developing and debugging

Run a native brick's tests from its folder (`clay tests/test_my_brick.clay`): a brick imports
itself by name, so the plugin is built on first use and cached in `build/` (not committed;
delete it to force a rebuild).

- The plugin is built with `-O2 -fPIC -shared -w`: no debug symbols, no warnings. To debug,
  add `"-g", "-O0"` to `cflags` in `brick.json` (the brick's flags come last, so they win) and
  run under a debugger: `lldb -- clay tests/test_my_brick.clay`, or `gdb --args clay ...`.
  Breakpoints on the plugin's functions can be set before the program starts.
- To see warnings, compile by hand:
  `cc -Wall -Wextra -fsyntax-only -I<clay>/src/runtime native/my_brick.c`.
- For memory errors, build Clay with
  `-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"`: the brick is built with the
  same flags (through `clay-native.cfg`).
- Print with `api->write_output(text)` (API version 3), not `printf`: under `--dap` stdout is
  the debugger's channel and `printf` output is lost.
- `clay --compile` links the brick's sources into the executable, so the same tests can run
  compiled: `clay --compile -o t tests/test_my_brick.clay && ./t`.
- Test the C on its own, in a small `main()`, before wrapping it, when the logic is the
  hard part.

The brick's `main` file wraps the plugin's prototypes in a Clay API. See
[clay-ann](https://github.com/mseminatore/clay-ann), neural networks over the
libann C library, for a complete example. `clayc`, the Clay compiler written in
Clay, supports native bricks too: it shares the same `build/` cache, and finds
the C compiler through `--cfg`, `CLAY_NATIVE_CFG`, or the Clay build it came from.

---

[← Guide index](README.md)
