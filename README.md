# Clay

Clay is a prototype-based, pure object-oriented programming language that compiles to human-readable C. Everything is an object. Objects communicate via message passing. New objects are created by cloning existing ones and adding or modifying slots and methods.

## Building

Requires CMake 3.10+ and a C compiler. On Windows, Visual Studio is detected automatically.

```
cmake -S . -B build
cmake --build build
```

This produces:
- `build/Debug/clay.exe` — the Clay compiler
- `build/Debug/test_runner.exe` — runtime unit tests

## Usage

```
clay [options] <source.clay>
clay              # start interactive REPL
```

| Option | Description |
|--------|-------------|
| `-c` | Emit C code only (don't compile to binary) |
| `-o <name>` | Set output binary name |
| `--ast` | Print the AST and exit |
| `--repl` | Start interactive REPL (also activates with no arguments) |

### REPL

Run `clay` with no arguments to start the interactive REPL:

```
$ clay
Clay REPL (type 'quit' or Ctrl+Z to exit)

clay> 1 + 2
3
clay> name = "World"
clay> "Hello, ${name}!"
Hello, World!
clay> Point = Object clone({
  ...   x = 0
  ...   y = 0
  ... })
clay> quit
Bye!
```

Variables and object definitions persist across inputs. Bare expressions are automatically printed. Multi-line input is detected via brace/paren matching.

### Examples

```sh
# Compile and run
clay hello.clay
./hello

# Inspect the generated C
clay -c hello.clay
cat hello.c

# Dump the AST
clay --ast hello.clay
```

## Language

### Everything is an object

Integers, strings, booleans, arrays, blocks — all objects. All operations are message sends.

```clay
42 println()          // send 'println' to an integer
"hello" length()      // send 'length' to a string
```

### Message passing

Messages use IO-style syntax: `receiver message(args)`. Unary messages have no parentheses.

```clay
"hello" println()         // unary with parens
"hello" length()          // returns 5
arr at(0)                 // message with one arg
arr each({ |x| x println() })  // message with block arg
```

### Binary operators

Binary operators (`+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`) are messages sent to the left operand.

```clay
3 + 4         // sends '+' to 3 with arg 4
10 > 5        // sends '>' to 10, returns true
```

Standard math precedence applies.

### Prototypes and cloning

Objects are created by cloning a prototype and defining slots (data) and methods:

```clay
Point = Object clone({
  x = 0
  y = 0

  moveTo(newX, newY) {
    x = newX
    y = newY
  }

  toString() {
    "(" + x toString() + ", " + y toString() + ")"
  }
})

p = Point clone()
p moveTo(3, 4)
p toString() println()   // (3, 4)
```

Inside methods, bare identifiers are looked up as slots on `self`. Parameters shadow slot names.

### Blocks

Blocks are closures. Parameters go between pipes:

```clay
doubler = { |x| x * 2 }
doubler value(5) println()   // 10

// No-arg block
greet = { "hello" println() }
greet value()
```

### Control flow

Booleans are objects. Control flow is message passing with blocks:

```clay
(x > 0) ifTrue({ "positive" println() })
(x > 0) ifFalse({ "non-positive" println() })

// Chaining ifTrue/ifFalse
(x > 0) ifTrue({ "positive" }) ifFalse({ "non-positive" })
```

Blocks capture variables from their enclosing scope (closures):

```clay
multiplier(factor) {
  { |x| x * factor }   // 'factor' is captured from the method scope
}
```

### Imports

Use `import` to include definitions from another Clay file:

```clay
import("math.clay")        // relative to the importing file
import("lib/utils.clay")   // subdirectory paths work too
```

Imports are resolved at parse time. Duplicate imports of the same file are silently skipped.

### Unary minus

Negative numbers work naturally:

```clay
x = -5
y = -(a + b)
```

### String interpolation

Use `${}` inside strings to embed expressions:

```clay
name = "Clay"
"Hello, ${name}!" println()       // Hello, Clay!
"2 + 3 = ${2 + 3}" println()     // 2 + 3 = 5
"${x} + ${y} = ${x + y}" println()
```

Expressions are automatically converted via `toString()`.

### Logical operators

```clay
(a && b)     // short-circuit AND
(a || b)     // short-circuit OR
!a           // logical NOT (sends 'not' message)
```

`&&` and `||` short-circuit: the right side is only evaluated if needed. Values are truthy unless they are `nil`, `false`, or `0`.

### While loops

Blocks support `whileTrue` and `whileFalse` for looping:

```clay
i = 0
{ i < 10 } whileTrue({
  i println()
  i = i + 1
})
```

### Arrays

```clay
arr = [1, 2, 3]
arr at(0)                     // 1
arr append(4)
arr size()                    // 4
arr each({ |item| item println() })
```

### Strings

```clay
s = "hello world"
s length()                          // 11
s at(0)                             // "h"
s contains("world")                 // true
s slice(0, 5)                       // "hello"
s split(" ")                        // ["hello", "world"]
s replace("world", "Clay")          // "hello Clay"
```

### Comments

```clay
// line comment
/* block comment */
```

### Error handling

Clay provides `try`/`catch` blocks and `raise` for error handling:

```clay
// Catch errors
result = try({
  10 / 0
}) catch({ |e|
  "Caught: " + e message()
})

// Raise custom errors
raise("something went wrong")

// Errors have message() and type() methods
try({
  nil banana()
}) catch({ |e|
  e type() println()    // "NilError"
  e message() println() // "message 'banana' sent to nil"
})
```

Error types include `DivisionByZero`, `MethodNotFound`, `NilError`, `TypeError`, and `RuntimeError` (for user-raised strings).

Objects can override `doesNotUnderstand` for proxy-style dispatch:

```clay
Proxy = Object clone({
  doesNotUnderstand(name, args) {
    "forwarding: " + name
  }
})
```

## Core Objects

| Prototype | Methods |
|-----------|---------|
| Object | `clone()`, `toString()`, `println()`, `doesNotUnderstand(name, args)` |
| Number | `+`, `-`, `*`, `/`, `%`, `<`, `>`, `<=`, `>=`, `==`, `!=`, `toString()`, `println()` |
| String | `+`, `==`, `length()`, `at(i)`, `contains(sub)`, `slice(start, end)`, `split(delim)`, `replace(old, new)`, `print()`, `println()`, `toString()` |
| Array | `at(i)`, `append(val)`, `size()`, `each(block)`, `toString()` |
| Block | `value(args...)`, `whileTrue(block)`, `whileFalse(block)` |
| Boolean | `ifTrue(block)`, `ifFalse(block)`, `not()`, `toString()` |
| Error | `message()`, `type()`, `toString()` |

## Generated C

Clay generates readable C. This Clay code:

```clay
p = Point clone()
p moveTo(3, 4)
```

Generates:

```c
ClayValue p = clay_send_val(Point, "clone", 0, NULL);
clay_send_val(p, "moveTo", 2, (ClayValue[]){ clay_int(3), clay_int(4) });
```

The generated C includes `#line` directives mapping back to the `.clay` source, so debuggers (gdb, lldb, Visual Studio) step through Clay code.

## Architecture

```
.clay source → [Lexer] → [Parser] → [CodeGen] → .c file → [C Compiler] → binary
                                                                ↑
                                                         Clay Runtime (linked)
```

The runtime is a static C library providing the object model, message dispatch, reference counting, and core prototypes.

## Dependencies

Included as git submodules under `deps/`:

- [mseminatore/hash](https://github.com/mseminatore/hash) — hash table for object slots and methods
- [mseminatore/testy](https://github.com/mseminatore/testy) — test framework for runtime unit tests

## Editor Support

### VS Code

A syntax highlighting extension is included at `editors/vscode/clay-lang/`. To install:

```powershell
# Windows (PowerShell as Admin) — create a junction
$ext = "$env:USERPROFILE\.vscode\extensions\clay-lang"
New-Item -ItemType Junction -Path $ext -Target (Resolve-Path "editors\vscode\clay-lang").Path
```

Then restart VS Code. See [`editors/vscode/clay-lang/README.md`](editors/vscode/clay-lang/README.md) for details.
