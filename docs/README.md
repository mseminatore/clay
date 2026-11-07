# The Clay guide

Clay is a prototype-based, pure object-oriented language: everything is an
object, objects talk by sending messages, and new objects are made by cloning
existing ones. Programs run instantly in a bytecode VM or compile to C for
native executables.

1. **[Getting started](getting-started.md)** — build Clay, write and run a
   first program, the REPLs, and the bundled example programs.
2. **[Language tour](language-tour.md)** — objects and cloning, slots and
   methods, `self` and `super`, blocks and closures, control flow, operators,
   strings, numbers, arrays, Dicts, errors, imports.
3. **[Core objects](core-objects.md)** — the built-in prototypes: `System`,
   `File`, `Date`, `Http`, `Regex`, `Fiber`, `Vec3`, `Thread`, `Graphics`,
   `Db`, and a reference table of every built-in method.
4. **[Bricks](bricks.md)** — the package manager: `brick.json`, installing and
   publishing bricks, and the bundled standard library (`stdlib`) and web bricks (`web`).
5. **[Native compilation, FFI and plugins](native-and-plugins.md)** —
   `clay --compile`, the generated C, the architecture, calling C through FFI,
   and writing plugins.
6. **[Debugging and editor support](debugging.md)** — tracing, the stepper,
   VS Code debugging, and the VS Code extension.
7. **[Known limitations](limitations.md)** — rough edges in this release and
   how to work around them.

Every runnable code sample in this guide is executed by the test suite
(`ctest -L docs`), so the examples stay correct as the language evolves.
