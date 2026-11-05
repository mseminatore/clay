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

CLAY_PLUGIN_EXPORT void clay_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "MathEx", methods, NULL, 0 };
    api->register_type(&def);
}
```

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

---

[← Guide index](README.md)
