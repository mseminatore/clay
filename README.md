# Welcome to the Clay language
[![CI](https://github.com/mseminatore/clay/actions/workflows/ci.yml/badge.svg)](https://github.com/mseminatore/clay/actions/workflows/ci.yml)

Clay is a prototype-based, pure object-oriented programming language. Like many pure OOP languages, everything is a descendant of `Object`. Objects communicate using message passing (message sends). Rather than defining static class hierarchies, new objects are created by cloning existing ones and adding or modifying slots (data members) and methods. 

The Clay language borrows much of its syntax from Smalltalk. Particularly message sends, message chaining and blocks. But it is also influenced by Self, Io and elements of Ruby.

Clay programs can run in an embedded bytecode VM, or compile to C for native executables.


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

// Blocks are closures (lambda functions)
evens = (1..10) filter { |n| n % 2 == 0 }

// Collections take blocks
evens map { |n| n * n } println              // [4, 16, 36, 64, 100]

// Definite loops
5 times { |i| i println }                    // 1\n 2\n 3\n 4\n 5\n
(1..5) each { |i| i println }                // 1\n 2\n 3\n 4\n 5\n

// Error objects are raised and can be caught
r = try { 10 / 0 } catch { |e| e type }
r println                                    // DivisionByZero
```

**Key Language Features:** 
- messages are sent to objects
- prototypes instead of classes
- block closures, fibers and threads
- standard library of bricks (JSON, CSV, HTTP
server, ORM, templates, …)
- a Git-based package manager
- native compilation to C for speed and distribution 
- a VS Code extension with a full debugger.

## Installing Clay

Download the right platform archive from the
[releases page](https://github.com/mseminatore/clay/releases) (Linux x86_64 and arm64, macOS
arm64, Windows x86_64), unpack it anywhere, and add its `bin` folder to your `PATH`:

```sh
curl -LO https://github.com/mseminatore/clay/releases/latest/download/clay-<version>-macos-arm64.tar.gz
tar xzf clay-<version>-macos-arm64.tar.gz
export PATH="$PWD/clay-<version>-macos-arm64/bin:$PATH"
clay --version
```

The archive includes the standard library and web bricks, plus what `clay --compile` and native
bricks link against (if you need to build native bricks you must have a C compiler). [Getting started](docs/getting-started.md#installing)
covers which compiler to install, and the macOS and Windows download warnings.

## Building Clay

Requires CMake 3.10+ and a C compiler. On Windows, Visual Studio should be detected automatically.
OpenSSL (HTTPS) and SDL2 (`Graphics`) are optional and picked up when installed.

```
git clone --recurse-submodules https://github.com/mseminatore/clay.git
cd clay
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces `build/clay` (under `build/Debug/` as an `.exe` on Windows).

## Quick start

```sh
clay hello.clay                  # run a program (bytecode VM)
clay --compile hello.clay        # build a native executable ./hello
clay                             # interactive REPL
clay bricks install              # install dependencies from brick.json
clay --version                   # the Clay version
clay --help                      # list all options
clay bricks --help               # show brick subcommands
```

## Documentation

The [Clay guide](docs/README.md) covers a range of topics:

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
