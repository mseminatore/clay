# clay-codegen

The Clay-to-C code generator, written in Clay — a port of
`src/compiler/codegen.c` that produces the same C byte for byte. CTest
(`clay_selfhost_codegen`, and `clay_selfhost_codegen_native`) compares it
with `clay -c` on every `.clay` file in the repository plus the cases in
`test/cases/`.

```clay norun
import("../clay-codegen/src/main.clay")

p = Parser new(File read("prog.clay"), "prog.clay")
program = p parseProgram
c = (CodeGen new("prog.clay", System executable)) generate(program)
File write("prog.c", c)
```

`CodeGen new(sourcePath, pluginExe)`: `sourcePath` is used for `#line`
directives and runtime error locations; `pluginExe` is the clay executable
whose `plugins/` folder the program loads at startup (nil: none). Plugin
prototypes known at compile time come from `System pluginGlobals`.

The generated C follows the native backend's conventions (see
`docs/native-and-plugins.md`): every value is a `ClayValue`, methods and
blocks become static C functions, closures capture variables in `ClayCell`
boxes, and reference counts are managed explicitly.

```sh
clay selfhost/clay-codegen/tools/c.clay prog.clay prog.c   # like clay -c -o prog.c prog.clay
```
