# clay-bytecode

The Clay-to-bytecode compiler, written in Clay — a port of
`src/compiler/bc_compiler.c` that produces the same bytecode byte for byte.
CTest (`clay_selfhost_bytecode`, and `clay_selfhost_bytecode_native`)
compares it with `clay --bytecode` on every `.clay` file in the repository
plus the cases in `test/cases/`.

```clay norun
import("../clay-bytecode/src/main.clay")

p = Parser new(File read("prog.clay"), "prog.clay")
program = p parseProgram
bc = BcGen new("prog.clay", true)
unit = bc compile(program)      // nil after a compile error
VM run(unit)                    // the value of the program's last expression
```

`BcGen new(sourcePath, live)`: `sourcePath` is used for error locations;
`live` true compiles for this process's VM (it asks the VM which globals
exist and where its constant pool, chunks and method slots end), false
compiles for a fresh one (`dump`, the comparison with `clay --bytecode`).

A `BcGen` keeps its state between compiles, like the C compiler in the REPL:
the constant pool, chunks, method slots and the top-level variables it has
seen. Each `compile` returns a *unit* with only what it added —
`[constBase, constants, chunkBase, chunks, methodBase, methods, code, lines,
file]` — so a REPL compiles one input after another into the same VM.

The bytecode is the VM's (opcodes from `src/vm/clay.yaml`); every Clay
operation — send, clone, blocks, methods, try/catch — is a call to one of the
native functions of the runtime bridge (`src/runtime/vm_bridge.c`), numbered
in the order `bc_compiler_init` registers them. `VM run` needs the VM linked
in, so it works in native programs (`clay --compile`), not inside `clay`'s
own VM.

```sh
clay selfhost/clay-bytecode/tools/bc.clay prog.clay   # like clay --bytecode prog.clay
```
