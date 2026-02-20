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
clay [options] [source.clay]
clay              # start VM REPL (default)
```

| Option | Description |
|--------|-------------|
| `-c` | Emit C code only (don't compile to binary) |
| `-o <name>` | Set output binary name |
| `--ast` | Print the AST and exit |
| `--vm-repl` | Launch bytecode VM REPL (instant evaluation, default) |
| `--repl` | Launch classic REPL (compiles to C each input) |
| `-h`, `--help` | Show help message |

### VM REPL

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
>>> [1, 2, 3] each({ |n| n println() })
1
2
3
=> [1, 2, 3]
>>> (10 > 5) ifTrue({ "yes" }) ifFalse({ "no" })
=> yes
>>> Point = Object clone({
...   x = 0
...   y = 0
...   getX() { self x() }
... })
>>> p = Point clone({})
>>> p getX()
=> 0
>>> quit
Bye!
```

Variables and object definitions persist across inputs. Bare expressions are automatically printed. Multi-line input is detected via brace/paren matching. Errors are caught and reported without exiting.

### Classic REPL

The classic REPL (`clay --repl`) accumulates source and recompiles to C on each input. It supports the full language but is slower due to C compiler invocation on every line.

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

Messages use IO-style syntax: `receiver message(args)`. Parentheses are optional for zero-argument messages.

```clay
"hello" println          // no parens (preferred for unary)
"hello" println()        // parens also work
"hello" length()         // returns 5
arr at(0)                // message with one arg
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
arr first                     // 1
arr last                      // 3
arr each({ |item| item println() })

// Functional methods
arr map({ |x| x * 2 })       // [2, 4, 6]
arr filter({ |x| x > 1 })    // [2, 3]
arr reduce(0, { |acc, x| acc + x })  // 6
arr find({ |x| x > 2 })      // 3

// Utility methods
arr reverse                   // [3, 2, 1]
arr sort                      // [1, 2, 3]
arr join(", ")                // "1, 2, 3"
arr contains(2)               // true
arr indexOf(2)                // 1
arr removeAt(0)               // removes first element
```

### Numbers

Integers are tagged values (fast, no allocation). Floats are heap-allocated objects. Mixed operations promote to float.

```clay
42                            // integer
3.14                          // float
10 / 3                        // 3 (integer division)
10 toFloat / 3                // 3.33333 (float division)
3.14 + 2                      // 5.14 (mixed → float)
3.14 toInt                    // 3 (truncation)
```

### Strings

Strings support C-style escape sequences: `\n` (newline), `\t` (tab), `\r` (carriage return), `\\` (backslash), `\"` (quote).

```clay
s = "hello world"
s length()                          // 11
s at(0)                             // "h"
s contains("world")                 // true
s slice(0, 5)                       // "hello"
s split(" ")                        // ["hello", "world"]
s replace("world", "Clay")          // "hello Clay"
s toInt                             // error (not a number)
"42" toInt                          // 42
"3.14" toFloat                      // 3.14
"  hello  " trim                    // "hello"
"hello" upper                       // "HELLO"
"HELLO" lower                       // "hello"
"hello" startsWith("he")            // true
"hello" endsWith("lo")              // true
"hello" reverse                     // "olleh"
"line1\nline2" println              // prints two lines
"col1\tcol2" println                // tab-separated
```

### Comments

```clay
// line comment
/* block comment */
```

### Dict (HashMap)

```clay
d = Dict clone()
d set("name", "Clay")
d set("version", 1)

d get("name") println()     // "Clay"
d has("name") println()     // true
d size() println()          // 2
d remove("version")

// Iteration
d each({ |key, value|
  "${key}: ${value toString()}" println()
})

// Inspect
d keys() println()          // [name]
d values() println()        // [Clay]
d toString() println()      // {name: Clay}
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
| Object | `clone()`, `toString()`, `println()`, `type()`, `methods()`, `slots()`, `inspect()`, `doesNotUnderstand(name, args)` |
| Number | `+`, `-`, `*`, `/`, `%`, `<`, `>`, `<=`, `>=`, `==`, `!=`, `toFloat()`, `toInt()`, `toString()`, `println()` |
| String | `+`, `==`, `length()`, `at(i)`, `contains(sub)`, `slice(start, end)`, `split(delim)`, `replace(old, new)`, `toInt()`, `toFloat()`, `trim()`, `upper()`, `lower()`, `startsWith(s)`, `endsWith(s)`, `reverse()`, `print()`, `println()`, `toString()` |
| Array | `at(i)`, `append(val)`, `size()`, `length()`, `each(block)`, `map(block)`, `filter(block)`, `reduce(init, block)`, `find(block)`, `sort()`, `reverse()`, `first()`, `last()`, `join(sep)`, `contains(val)`, `indexOf(val)`, `removeAt(i)`, `toString()` |
| Block | `value(args...)`, `whileTrue(block)`, `whileFalse(block)` |
| Boolean | `ifTrue(block)`, `ifFalse(block)`, `not()`, `toString()` |
| Error | `message()`, `type()`, `toString()` |
| Dict | `set(key, val)`, `get(key)`, `has(key)`, `remove(key)`, `size()`, `keys()`, `values()`, `each(block)`, `toString()` |
| Math | `abs(n)`, `sqrt(n)`, `pow(b,e)`, `min(a,b)`, `max(a,b)`, `floor(n)`, `ceil(n)`, `round(n)`, `sin(n)`, `cos(n)`, `random()`, `pi`, `e` |
| Console | `readLine()`, `prompt(msg)` |
| File | `read(path)`, `readLines(path)`, `write(path, content)`, `append(path, content)`, `exists(path)` |

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
.clay source → [Lexer] → [Parser] → [AST] ─┬→ [C CodeGen]  → .c → [C Compiler] → binary
                                            │                          ↑
                                            │                   Clay Runtime (linked)
                                            │
                                            └→ [BC CodeGen] → bytecode → [VM] → result
                                                                          ↑
                                                                   Clay Runtime (bridged)
```

**Compile mode** (`clay file.clay`): generates human-readable C with `#line` directives, invokes the system C compiler, and links the Clay runtime as a static library.

**VM REPL mode** (`clay` or `clay --vm-repl`): compiles expressions to bytecode and executes them instantly in an embedded bytecode VM. The VM bridges to the same Clay runtime via native functions, so all core objects and methods work identically.

## Dependencies

Included as git submodules under `deps/`:

- [mseminatore/hash](https://github.com/mseminatore/hash) — hash table for object slots and methods
- [mseminatore/testy](https://github.com/mseminatore/testy) — test framework for runtime unit tests

## Editor Support

### VS Code

**Syntax highlighting:** A TextMate grammar extension is included at `editors/vscode/clay-lang/`. To install:

```powershell
# Windows (PowerShell as Admin) — create a junction
$ext = "$env:USERPROFILE\.vscode\extensions\clay-lang"
New-Item -ItemType Junction -Path $ext -Target (Resolve-Path "editors\vscode\clay-lang").Path
```

Then restart VS Code. See [`editors/vscode/clay-lang/README.md`](editors/vscode/clay-lang/README.md) for details.

**Build, run & debug:** The `.vscode/` folder provides ready-to-use integration (requires CMake Tools and C/C++ extensions):

| Feature | How |
|---------|-----|
| Build all | `Ctrl+Shift+B` (default build task) |
| Run tests | `Ctrl+Shift+P` → "Run Test Task" |
| Debug test runner | Launch: "Debug: Test Runner" |
| Debug VM REPL | Launch: "Debug: VM REPL" |
| Debug compiled .clay | Launch: "Debug: Run Compiled .clay Program" |
| Compile current .clay | Task: "Compile Current .clay File" |

In compiled mode, `#line` directives map generated C back to `.clay` source lines, so the Visual Studio debugger steps through Clay code directly.
