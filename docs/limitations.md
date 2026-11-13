# Known limitations

Clay is young. These are the rough edges we know about, what they look like
when you hit them, and how to work around them. Each is tracked for a future
release.

## Language

**A top-level variable can hide an inherited slot inside methods.** A slot
declared in an object's own `clone` body always wins over a top-level variable
of the same name. A slot the object only inherits from a prototype isn't known
when the method is compiled, so there the top-level variable wins:

```clay norun
Base = Object clone { path = "/etc/app.conf" }
Sub = Base clone {
    show() { path }            // the inherited slot isn't declared here
}
path = "./local.conf"
Sub show println               // ./local.conf
```

Write `self path` to read such a slot explicitly, or redeclare it in the
clone body.

**Top-level variables are shared by every file.** `import` has no file scope: the top-level
variables of a program and of everything it imports, bricks included, are one set of globals.
Two files that each assign `count` at their top level are using the same variable, so an
import can replace a value the importing file set earlier:

```clay norun
// lib.clay
count = 0

// main.clay
count = "mine"
import("lib.clay")
count println                  // 0
```

Blocks and methods are safe from this: a method's assignment never changes a top-level
variable, and a top-level block's changes only those of its own file (see
[Imports](language-tour.md#imports)). So keep a brick's own top-level names few and
distinctive, typically just the objects it provides. `import("lib.clay") as Lib` gives the
importer a namespace to read, but the imported file's top-level variables are still created
as globals.

**Recursion depth is limited by the C stack.** Every Clay method or block call that
recurses uses C stack. Programs run on a stack of 256 MB, which allows around 60,000 nested
method calls (somewhat fewer through blocks); beyond that, or in a fiber, which has a 256 KB
stack of its own (about 60 nested calls), the program raises a `RuntimeError`, "stack overflow:
recursion is too deep", that `try`/`catch` can handle. There is no tail-call elimination: loop
with `whileTrue` or `each` instead of recursing for long sequences.

Set the environment variable `CLAY_STACK_MB` to change the size: `CLAY_STACK_MB=1024` for deeper
recursion, `CLAY_STACK_MB=16` for a leaner process, or `CLAY_STACK_MB=0` to use the operating
system's own stack size (8 MB on Linux and macOS, 1 MB on Windows). Only the pages a program
uses are committed, so a big stack costs address space, not memory, until recursion reaches it.
It applies to `clay`, to compiled programs and to `clayc`.

**Socket calls block everything.** `Socket` reads, writes and accepts stop the whole
program (the VM, fibers and all) until they finish or time out. Give sockets a timeout
(`setTimeout`) and use `Socket select` to serve several at once. There is no TLS on raw sockets.

**A method takes at most 16 parameters.** A 17th parameter isn't bound: using
it fails with `... does not understand 'a16'`. Pass a Dict or an object
instead of a long parameter list.

**Most built-in methods ignore extra arguments.** Methods written in Clay raise
`ArgumentError` when called with too many arguments, but those implemented in C
(other than `print` and `println`) silently drop them: `Math sqrt(4, 9)` is `2`.

**String literals and interpolation:**

- A string literal ends at an embedded NUL: `"ab\x00cd"` is the two-character
  string `"ab"`.
- Inside `"${...}"`, a double quote ends the whole string, so
  `"v=${d get("k")}"` is a parse error. Compute the value first:
  `k = d get("k")`, then `"v=${k}"`.

**Reference cycles are never freed.** Clay counts references and has no cycle
collector, so objects that refer to each other (a parent with a `children`
array whose children point back with `parent`) stay alive after the last
outside reference goes. Break the cycle when you're done (`child parent = nil`)
in long-running programs. `System liveObjects` growing steadily is the sign
to look for.

**A bare `{` on the line after `clone` is read as the clone's body.** That is
how Allman-style bodies work (`Point = Object clone` then `{` alone on the
next line). A block statement written with its `{` alone on the line directly
after a `clone` line is therefore misread; put something after the `{`, or a
blank statement between them.

## Native compilation

**Native programs leak a little memory on non-local exits.** When an exception
or a block's `return` unwinds through compiled C frames, the temporaries and
locals of the frames it skips are not released (about one block object per
non-local return), and top-level globals are never freed at exit. This doesn't
affect results, only memory use in long-running programs that use non-local
returns heavily.

**`clay --compile` runs from a build tree.** `clay-native.cfg`, written next to
the `clay` binary by CMake, records paths inside the build directory, and there
is no `cmake --install` yet. Build Clay from source and run it from there.

## REPL

**A very long REPL session can run out of constants.** Every number and `nil`
literal typed into the VM REPL adds a constant, and a session has 65,536
constant slots; past that, values silently go wrong. Restart the REPL if a
session runs for a very long time.

## Self-hosting

**`clayc` running inside `clay`'s VM can only build native executables (and run
`bricks`).** Only one VM runs per process, so `clay selfhost/clayc.clay program.clay`
cannot run the program. Build a native `clayc` first
(`clay --compile -o clayc selfhost/clayc.clay`) to compile and run programs.
`clayc` also doesn't yet support MSVC; on Windows, use `clay --compile`.

---

[← Guide index](README.md)
