# Getting started

This page covers installing or building Clay, running programs, and the REPLs. When you're
ready, continue with the [language tour](language-tour.md).

## Installing

Each [release](https://github.com/mseminatore/clay/releases) has an archive per platform:
`clay-<version>-linux-x86_64.tar.gz`, `-linux-arm64.tar.gz`, `-macos-arm64.tar.gz` and
`-windows-x86_64.zip`, plus `SHA256SUMS` to check the download against. Unpack the archive anywhere
and add its `bin` folder to your `PATH`. It contains `bin/clay` and the bundled bricks in
`lib/clay/`, which `clay` finds on its own.

- **Linux**: needs glibc 2.35 or newer and OpenSSL 3 (Ubuntu 22.04+, Debian 12+, Fedora,
  RHEL 9).
- **macOS**: needs macOS 15 or newer on Apple silicon. The binary is not notarized, so if you
  downloaded it with a browser, macOS will refuse to open it. Download with `curl -LO`, or remove
  the quarantine flag once: `xattr -dr com.apple.quarantine clay-<version>-macos-arm64`.
- **Windows**: SmartScreen may warn about an unrecognized app. Choose **More info**, then **Run
  anyway**, or run `Unblock-File` on the zip in PowerShell before unpacking it. Or skip the
  manual steps and use the install script (see below).

### Windows install script

`tools/install.ps1` installs or updates Clay in one step. It downloads the latest Windows zip,
checks it against `SHA256SUMS`, installs it into `%LOCALAPPDATA%\Programs\clay` (no admin rights
needed) and adds its `bin` folder to your user `PATH`:

```powershell
irm https://raw.githubusercontent.com/mseminatore/clay/main/tools/install.ps1 | iex
```

Run the same command to update; it does nothing when you already have the latest version. It
replaces the whole install folder, and keeps the old install until the new `clay` runs, so a
failed update leaves the old version in place. To pass options, run it as a script block:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/mseminatore/clay/main/tools/install.ps1))) -Version 0.3.5
```

| Option | Meaning |
|---|---|
| `-Version x.y.z` | install this release instead of the latest |
| `-Prefix <dir>` | install somewhere else (gets `bin\`, `lib\clay\`, `share\`) |
| `-Force` | reinstall even if that version is already installed |
| `-NoModifyPath` | leave `PATH` alone |

From a clone of the repo: `powershell -ExecutionPolicy Bypass -File tools\install.ps1`.

Release builds have HTTPS but not `Graphics` (SDL2). `clay --compile` and native bricks
work from a release: `lib/clay/native/` holds the runtime headers and prebuilt libraries, and
your system's C compiler does the rest:

- **Linux**: `cc` (gcc or clang; `sudo apt install build-essential` on Debian/Ubuntu)
- **macOS**: the Xcode Command Line Tools (`xcode-select --install`)
- **Windows**: Visual Studio or its Build Tools with the "Desktop development with C++"
  workload. `clay` finds it on its own, with no Developer Command Prompt needed.

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

This produces `build/clay` (the compiler/VM) and `build/test_runner` (runtime unit tests) —
under `build/Debug/` with `.exe` suffixes on Windows / multi-config generators.
Add `build/` to your `PATH`, or call it as `./build/clay`.

For the fastest native programs, configure a Release build:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`.

To install Clay outside the source tree, run `cmake --install build --prefix ~/.local` (any
prefix works). This puts `clay` in `bin/` and the bundled bricks in `lib/clay/`, where `clay` finds
them on its own. `cpack` in the build directory makes the same tree as an archive
(`clay-<version>-<os>-<arch>.tar.gz`, or `.zip` on Windows). The install includes
`lib/clay/native/` (headers, static libraries and a `clay-native.cfg` with paths relative to it), so
an installed `clay` can `--compile` too.

## Your first program

Save this as `hello.clay`:

```clay
// A prototype with a slot and a method
Greeter = Object clone {
  greeting = "Hello"

  greet(name) {
    "${greeting}, ${name}!" println
  }
}

Greeter greet("world")                    // Hello, world!

// Clone it and override a slot
loud = Greeter clone
loud greeting = "HEY"
loud greet("Clay")                        // HEY, Clay!

// Blocks are closures; arrays take blocks
[1, 2, 3] map { |n| n * n } println       // [1, 4, 9]
```

Run it in the VM, or compile it to a native executable:

```sh
clay hello.clay                  # run (bytecode VM, starts instantly)
clay --compile hello.clay        # build ./hello (hello.exe on Windows)
./hello
```

## Usage

```
clay [options] [source.clay]
clay              # start VM REPL (default)
```

| Option | Description |
|--------|-------------|
| `-c` | Emit C code only (don't compile to binary) |
| `--compile` | Compile to native binary via C |
| `-o <name>` | Set output binary name (implies `--compile`) |
| `--ast` | Print the AST and exit |
| `--tokens` | Print the lexer's tokens (`<line> <type> <text>`) and exit |
| `--bytecode` | Print the compiled bytecode (constants, code, chunks, methods) and exit |
| `--dap` | Run as a DAP (Debug Adapter Protocol) server |
| `--vm-repl` | Launch bytecode VM REPL (instant evaluation, default) |
| `--repl` | Launch classic REPL (compiles to C each input) |
| `--trace` | VM REPL with bytecode tracing (shows each opcode) |
| `--step` | VM REPL with interactive stepping debugger |
| `--mem-stats` | Show memory allocation stats on REPL exit |
| `-h`, `--help` | Show help message |
| `bricks <command>` | Package manager — see [Bricks](bricks.md) |

## VM REPL

Run `clay` with no arguments to start the bytecode VM REPL. Expressions are compiled to bytecode and evaluated instantly — no C compiler invocation needed.

```
$ clay
Clay VM REPL v0.1 (type 'quit' or 'exit' to leave)
>>> 1 + 2
=> 3
>>> name = "World"
>>> "Hello, ${name}!"
=> Hello, World!
>>> doubler = { |x| x * 2 }
>>> doubler value(5)
=> 10
>>> [1, 2, 3] each { |n| n println }
1
2
3
=> [1, 2, 3]
>>> (10 > 5) ifTrue { "yes" } ifFalse { "no" }
=> yes
>>> Point = Object clone {
...   x = 0
...   y = 0
...   getX() { self x }
... }
>>> p = Point clone {}
>>> p getX
=> 0
>>> quit
Bye!
```

Variables and object definitions persist across inputs. Bare expressions are automatically printed. Multi-line input is detected via brace/paren matching. Errors are caught and reported without exiting.

## Classic REPL

The classic REPL (`clay --repl`) accumulates source and recompiles to C on each input. It supports the full language but is slower due to C compiler invocation on every line.

## Examples

```sh
# Run directly (bytecode VM, default)
clay hello.clay

# Compile to native binary
clay --compile hello.clay
./hello

# Emit C only (inspect the generated code)
clay -c hello.clay
cat hello.c

# Dump the AST
clay --ast hello.clay
```

### Mini Scheme interpreter

A full Scheme (R5RS subset) interpreter is included as `examples/scheme.clay` — roughly 1,000 lines of Clay demonstrating prototype chains, closures, deep recursion, Dict, error handling, and string parsing.

```sh
clay examples/scheme.clay              # interactive REPL
clay examples/scheme.clay -- --test    # run the 57-test suite
```

```
scheme> (define (fib n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))
scheme> (fib 10)
55
scheme> (map (lambda (x) (* x x)) '(1 2 3 4 5))
(1 4 9 16 25)
```

### Parallel ray tracer

A multi-threaded ray tracer using Vec3, Threads, and parallelMap. Optional `--live` flag opens a Graphics window showing progressive rendering.

```sh
clay examples/raytrace_parallel.clay                  # batch mode (parallel)
clay examples/raytrace_parallel.clay -- --live        # live Graphics window
clay examples/raytrace_parallel.clay -- --small       # 100x56
clay examples/raytrace_parallel.clay -- --hd          # 400x225
```

Requires building with `-DCLAY_THREAD_SAFE=ON` and SDL2 for `--live` mode. Output: `output.ppm`.

---

[← Guide index](README.md)
