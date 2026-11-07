# Clay
[![CI](https://github.com/mseminatore/clay/actions/workflows/ci.yml/badge.svg)](https://github.com/mseminatore/clay/actions/workflows/ci.yml)

Clay is a prototype-based, pure object-oriented programming language. Everything is an object. Objects communicate via message passing. New objects are created by cloning existing ones and adding or modifying slots and methods.

Clay programs run instantly in an embedded bytecode VM, or compile to human-readable C for native executables.

```clay
Animal = Object clone {
  name = "animal"
  sound() { "..." }
  speak() { "${name} says ${self sound}" println }
}

Dog = Animal clone {
  sound() { "Woof" }
}

rex = Dog clone
rex name = "Rex"
rex speak                                    // Rex says Woof

// Blocks are closures; collections take blocks
evens = (1..10) filter { |n| n % 2 == 0 }
evens map { |n| n * n } println              // [4, 16, 36, 64, 100]

// Errors are objects too
r = try { 10 / 0 } catch { |e| e type }
r println                                    // DivisionByZero
```

**Highlights:** message passing all the way down · prototypes instead of classes ·
closures, fibers and threads · a standard library of bricks (JSON, CSV, HTTP
server, ORM, templates, …) and a Git-based package manager · native compilation
through C · a VS Code extension with a full debugger.

## Building

Requires CMake 3.10+ and a C compiler. On Windows, Visual Studio is detected automatically.
OpenSSL (HTTPS) and SDL2 (`Graphics`) are optional and picked up when installed.

```
git clone --recurse-submodules https://github.com/mseminatore/clay.git
cd clay
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces `build/clay` (under `build/Debug/` with an `.exe` suffix on Windows and
other multi-config generators).

## Quick start

```sh
clay hello.clay                  # run a program (bytecode VM)
clay --compile hello.clay        # build a native executable ./hello
clay                             # interactive REPL
clay bricks install              # install dependencies from brick.json
clay --version                   # the Clay version
clay --help                      # all options
```

## Documentation

The [Clay guide](docs/README.md) covers everything in detail:

- [Getting started](docs/getting-started.md) — building, a first program, the REPLs, example programs
- [Language tour](docs/language-tour.md) — the syntax and object model
- [Core objects](docs/core-objects.md) — the built-in prototypes and their methods
- [Bricks](docs/bricks.md) — the package manager and standard library
- [Native compilation, FFI and plugins](docs/native-and-plugins.md)
- [Debugging and editor support](docs/debugging.md)

The examples in `examples/` range from small demos to a Scheme interpreter
(`examples/scheme.clay`) and a parallel ray tracer (`examples/raytrace_parallel.clay`).

## Dependencies

Bundled under `lib/` (git submodules; imported with no install step):

- [mseminatore/clay-stdlib](https://github.com/mseminatore/clay-stdlib) — the standard library, `import("stdlib")` or `import("stdlib/json")`
- [mseminatore/clay-web](https://github.com/mseminatore/clay-web) — HTTP server, router and ORM, `import("web")` or `import("web/router")`

Included under `deps/`:

- [mseminatore/hash](https://github.com/mseminatore/hash) — hash table for object slots and methods
- [mseminatore/testy](https://github.com/mseminatore/testy) — test framework for runtime unit tests
- [jcwangxp/Crossline](https://github.com/jcwangxp/Crossline) — cross-platform readline for the REPL (arrow keys, tab completion, history on all platforms)
- [SQLite](https://sqlite.org/) — embedded SQL database engine for the `Db` prototype
- [cJSON](https://github.com/DaveGamble/cJSON) — lightweight JSON parser for the DAP debug adapter
- [mseminatore/vmgen](https://github.com/mseminatore/vmgen) — generates the bytecode VM in `src/vm/generated/` from `src/vm/clay.yaml` (only needed when changing the VM: `cmake --build build --target regen_vm`)

## License

Clay is released under the [MIT License](LICENSE). Bundled and optional
third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
