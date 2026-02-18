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
```

| Option | Description |
|--------|-------------|
| `-c` | Emit C code only (don't compile to binary) |
| `-o <name>` | Set output binary name |
| `--ast` | Print the AST and exit |

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
```

### Arrays

```clay
arr = [1, 2, 3]
arr at(0)                     // 1
arr append(4)
arr size()                    // 4
arr each({ |item| item println() })
```

### Comments

```clay
// line comment
/* block comment */
```

## Core Objects

| Prototype | Methods |
|-----------|---------|
| Object | `clone()`, `toString()`, `println()`, `doesNotUnderstand(name, args)` |
| Number | `+`, `-`, `*`, `/`, `%`, `<`, `>`, `==`, `toString()`, `println()` |
| String | `+`, `==`, `length()`, `print()`, `println()`, `toString()` |
| Array | `at(i)`, `append(val)`, `size()`, `each(block)`, `toString()` |
| Block | `value(args...)` |
| Boolean | `ifTrue(block)`, `ifFalse(block)`, `toString()` |

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
