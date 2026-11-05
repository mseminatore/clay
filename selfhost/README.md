# Clay in Clay

A Clay compiler written in Clay. It compiles Clay to C exactly like
`clay --compile`, and it compiles itself.

| piece | what it is | checked against |
|---|---|---|
| [clay-lexer](clay-lexer) | tokens | `clay --tokens` |
| [clay-parser](clay-parser) | AST, desugaring, imports, syntax errors | `clay --ast` |
| [clay-codegen](clay-codegen) | C source | `clay -c` |
| [clayc.clay](clayc.clay) | the driver: source → C → native executable | the bootstrap |

Each piece is a port of its C counterpart in `src/compiler/` and agrees with
it exactly. CTest compares them on every `.clay` file in the repository plus
each brick's edge cases in `test/cases/` (label `selfhost`), both running in
the VM and compiled natively.

## Using it

```sh
clay selfhost/clayc.clay -o hello examples/hello.clay    # in the VM
./hello

clay --compile -o clayc selfhost/clayc.clay              # a native clayc
./clayc -o hello examples/hello.clay
```

`clayc` links against the runtime libraries named in the `clay-native.cfg`
that the CMake build writes next to `clay` (`--cfg` picks another). It
supports GCC/Clang builds; with MSVC use `clay --compile` for now.

## The bootstrap

`clay_selfhost_bootstrap` builds `clayc` three times:

1. `clay --compile` (the C compiler) builds `clayc1`;
2. `clayc1` (the Clay compiler) builds `clayc2`;
3. `clayc2` builds `clayc3`.

The C of generations 2 and 3 is identical — the fixpoint — and equal to the
C compiler's. `clayc3` then compiles and runs a test program.

What stays in C is the runtime (`src/runtime/`, which every Clay program
links against) and the system C compiler.
