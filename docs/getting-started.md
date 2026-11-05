# Getting started

This page covers building Clay, running programs, and the REPLs. When you're
ready, continue with the [language tour](language-tour.md).

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
