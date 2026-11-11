# Clay in Clay

A Clay compiler written in Clay. Like `clay`, it runs programs in the
bytecode VM, has a REPL, and builds native executables through C — with
exactly the bytecode and C the C compiler produces — and it compiles itself.

| piece | what it is | checked against |
|---|---|---|
| [clay-lexer](clay-lexer) | tokens | `clay --tokens` |
| [clay-parser](clay-parser) | AST, desugaring, imports, syntax errors | `clay --ast` |
| [clay-bytecode](clay-bytecode) | bytecode for the VM | `clay --bytecode` |
| [clay-codegen](clay-codegen) | C source | `clay -c` |
| [clay-bricks](clay-bricks) | the package manager (`bricks`) | `clay bricks`, step by step |
| [clayc.clay](clayc.clay) | the driver: run, REPL, native executables, `bricks` | running every test program, the bootstrap |

Each piece is a port of its C counterpart in `src/compiler/` and agrees with
it exactly. CTest compares them on every `.clay` file in the repository plus
each brick's edge cases in `test/cases/` (label `selfhost`), both running in
the VM and compiled natively.

## Using it

```sh
clay --compile -o clayc selfhost/clayc.clay    # build clayc (native)

./clayc examples/hello.clay                    # run a program in the VM
./clayc prog.clay a b                          # ... with arguments (System args)
./clayc                                        # the REPL
./clayc -o hello examples/hello.clay           # a native executable
./clayc -c hello.clay                          # the C only
./clayc --bytecode hello.clay                  # the bytecode, as clay --bytecode prints it
./clayc bricks add github.com/owner/repo       # the package manager, as clay bricks
```

Running a program compiles it to bytecode with clay-bytecode and hands it to
the VM through the runtime's `VM` prototype (`VM run(unit)`); the VM itself —
`src/vm/` and the bridge in `src/runtime/vm_bridge.c` — stays in C. That needs
a native clayc: inside `clay`'s VM (`clay selfhost/clayc.clay ...`) only
building native executables and `bricks` work.

Native executables link against the runtime libraries named in the
`clay-native.cfg` that the CMake build writes next to `clay` (`--cfg` picks
another). clayc supports GCC/Clang builds; with MSVC use `clay --compile` for
now.

## The bootstrap

`clay_selfhost_bootstrap` builds `clayc` three times:

1. `clay --compile` (the C compiler) builds `clayc1`;
2. `clayc1` (the Clay compiler) builds `clayc2`;
3. `clayc2` builds `clayc3`.

The C of generations 2 and 3 is identical — the fixpoint — and equal to the
C compiler's. `clayc3` then compiles a test program natively and runs it in
the VM. `clay_selfhost_run` runs every program in `test/programs` under both
`clay` and a native clayc and compares their output; `clay_selfhost_bricks`
runs the same `bricks` commands under `clay` and `clayc` against local git
repositories (no network) and compares what they print and write.

What stays in C is the runtime (`src/runtime/`, which every Clay program
links against) and the system C compiler.
