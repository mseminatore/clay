# Clay
[![CI](https://github.com/mseminatore/clay/actions/workflows/ci.yml/badge.svg)](https://github.com/mseminatore/clay/actions/workflows/ci.yml)

Clay is a prototype-based, pure object-oriented programming language. Everything is an object. Objects communicate via message passing. New objects are created by cloning existing ones and adding or modifying slots and methods.

Clay programs run instantly via an embedded bytecode VM, or can be compiled to human-readable C for native binaries.

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
| `--dap` | Run as a DAP (Debug Adapter Protocol) server |
| `--vm-repl` | Launch bytecode VM REPL (instant evaluation, default) |
| `--repl` | Launch classic REPL (compiles to C each input) |
| `--trace` | VM REPL with bytecode tracing (shows each opcode) |
| `--step` | VM REPL with interactive stepping debugger |
| `--mem-stats` | Show memory allocation stats on REPL exit |
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

### Debugging

**Bytecode tracing** (`--trace`) shows each VM opcode as it executes:

```
$ clay --trace
>>> 2 + 3
0000  PUSH_CONST      0
0003  PUSH_CONST      1
0006  PUSH_CONST      2
0009  CALL_NATIVE     768 3
0013  HALT
=> 5
```

**Interactive stepping** (`--step`) pauses at each source line:

```
$ clay --step
>>> x = 10
  1 | x = 10
[step] s
Stack (0):
[step] n
>>>
```

Step commands: `n` = next line, `c` = continue, `s` = show stack, `q` = quit.

**VS Code Debugging** (`--dap`) provides a full Debug Adapter Protocol server for VS Code:

- Set breakpoints by clicking in the gutter
- **Conditional breakpoints** — right-click a breakpoint, set a condition expression (e.g., `x > 10` or `name`)
- **Hit count breakpoints** — break only on the Nth hit (right-click → Edit Hit Count)
- Step through code line by line (Next, Step In, Step Out)
- **Pause** running programs with the pause button
- **Watch expressions** — evaluate variables and dot-notation (e.g., `obj.slot.nested`) in the debug console and on hover
- Inspect global variables in the Variables panel (expand objects and arrays)
- View the call stack
- Pause on exceptions

To use: install the Clay extension from `editors/vscode/clay-lang/`, add a launch configuration:

```json
{
  "type": "clay",
  "request": "launch",
  "name": "Debug Clay",
  "program": "${file}",
  "clayPath": "path/to/clay"
}
```

Then press F5 to start debugging.

### Classic REPL

The classic REPL (`clay --repl`) accumulates source and recompiles to C on each input. It supports the full language but is slower due to C compiler invocation on every line.

### Examples

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

#### Mini Scheme interpreter

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

#### Parallel ray tracer

A multi-threaded ray tracer using Vec3, Threads, and parallelMap. Optional `--live` flag opens a Graphics window showing progressive rendering.

```sh
clay examples/raytrace_parallel.clay                  # batch mode (parallel)
clay examples/raytrace_parallel.clay -- --live        # live Graphics window
clay examples/raytrace_parallel.clay -- --small       # 100x56
clay examples/raytrace_parallel.clay -- --hd          # 400x225
```

Requires building with `-DCLAY_THREAD_SAFE=ON` and SDL2 for `--live` mode. Output: `output.ppm`.

## Language

### Everything is an object

Integers, strings, booleans, arrays, blocks — all objects. All operations are message sends.

```clay
42 println              // send 'println' to an integer
"hello" length          // send 'length' to a string
```

### Message passing

Messages use IO-style syntax: `receiver message(args)`. Parentheses are optional for zero-argument messages.

```clay
"hello" println          // no parens (preferred for unary)
"hello" println()        // parens also work
"hello" length           // returns 5
arr at(0)                // message with one arg
arr each { |x| x println }  // message with trailing block
```

### Binary operators

Binary operators (`+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`) are messages sent to the left operand.

```clay
3 + 4         // sends '+' to 3 with arg 4
10 > 5        // sends '>' to 10, returns true
```

Standard math precedence applies.

### Operator overloading

Custom types can define operator methods. Define them inside clone bodies or use `setMethod`:

```clay
Vec = Object clone {
  x = 0
  y = 0
  init(vx, vy) { self x = vx; self y = vy }

  +(other) { Vec new(x + other x, y + other y) }
  ==(other) { x == other x && y == other y }

  toString() { "Vec(" + x toString + ", " + y toString + ")" }
}

v1 = Vec new(1, 2) + Vec new(3, 4)   // Vec(4, 6)
v1 == Vec new(4, 6)                   // true

// Or use setMethod for external definition
Vec setMethod("*", { |scalar| Vec new(self x * scalar, self y * scalar) })
Vec new(2, 3) * 5   // Vec(10, 15)
```

Overloadable operators: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`. For built-in numbers, the fast integer/float path is used automatically; the custom method is only called for non-numeric types.

### Prototypes and cloning

Objects are created by cloning a prototype and defining slots (data) and methods:

```clay
Point = Object clone {
  x = 0
  y = 0

  moveTo(newX, newY) {
    x = newX
    y = newY
  }

  toString() {
    "(" + x toString + ", " + y toString + ")"
  }
}

p = Point clone
p moveTo(3, 4)
p toString println   // (3, 4)
```

Inside methods, bare identifiers like `x` resolve to slots on `self` — both for reads and writes. If `x` is a slot defined in the enclosing `clone` block, then `x = val` writes to the slot (Io-style `updateSlot` semantics). Truly new names (not matching any slot) create method-local variables. Use `self x = val` when a parameter shadows a slot name, as in the `init` pattern below.

### Super

Use `super` to call the parent prototype's version of a method:

```clay
Animal = Object clone {
    speak() { "..." }
}

Dog = Animal clone {
    speak() { super speak + " Woof!" }
}

Dog speak println   // ... Woof!
```

Super works correctly through multi-level chains:

```clay
A = Object clone { greet() { "A" } }
B = A clone { greet() { super greet + "B" } }
C = B clone { greet() { super greet + "C" } }
C greet println   // ABC
```

`super` dispatches from the prototype where the method was *defined*, not from `self`, so it is safe with deeply cloned instances.

### Slot access

Read a slot by sending its name as a zero-argument message:

```clay
p = Point clone
p x             // => 0 (reads the 'x' slot)
```

Write a slot with assignment syntax on the receiver:

```clay
p x = 42        // sets slot 'x' to 42
p x             // => 42
```

Slot reads walk the prototype chain — if the object doesn't have the slot, its parent is checked, and so on.

### Implicit self in methods

Inside a method body, bare identifiers that aren't local variables automatically resolve to `self` slots. This means you can write:

```clay
Point = Object clone {
  x = 0
  y = 0
  toString() { "(" + x toString + ", " + y toString + ")" }
}
```

The lookup order is: local variable → upvalue (captured variable) → `self` slot → global. Slot writes also resolve implicitly — if a name matches a slot defined in the enclosing `clone` block, `x = val` writes to the slot (Io-style `updateSlot` semantics):

```clay
Point moveTo(nx, ny) {
  x = nx       // writes to slot — no self needed
  y = ny       // same here
}
```

Use explicit `self` when a parameter name shadows a slot name:

```clay
Point init(x, y) {
  self x = x   // self needed because parameter 'x' shadows slot 'x'
  self y = y
}
```

### Dynamic Methods

Methods can be added to objects after cloning by assigning a block to a slot. The block is automatically invoked when the slot is accessed via message passing (Io-style):

```clay
Dog = Object clone {
  name = "Rex"
}

Dog speak = { self name + " says Woof!" }
Dog speak println     // Rex says Woof!

// With arguments
Dog greet = { |other| self name + " greets " + other }
Dog greet("Buddy") println  // Rex greets Buddy

// Methods added this way inherit through the prototype chain
puppy = Dog clone
puppy name = "Buddy"
puppy speak println   // Buddy says Woof!
```

Note: blocks assigned to slots must use explicit `self` to access other slots, since the compiler doesn't know the block will be used as a method.

Use `getSlot("name")` to retrieve the raw block without invoking it:

```clay
block = Dog getSlot("speak")   // returns the block
block value println         // Rex says Woof!
```

### Constructors

Define an `init` method on a prototype, then create instances with `new`:

```clay
Point = Object clone {
  x = 0
  y = 0
  init(x, y) {
    self x = x
    self y = y
  }
}

p = Point new(3, 4)
p x             // => 3
p y             // => 4
```

#### Default parameters

Methods can have default values for trailing parameters:

```clay
greet(name, greeting = "Hello") {
  "${greeting}, ${name}!" println
}
greet("world")             // Hello, world!
greet("world", "Hi")       // Hi, world!
```

`new` clones the prototype and calls `init(args...)` on the clone if an `init` method exists. If there is no `init`, `new` just clones:

```clay
Cat = Object clone { name = "kitty" }
c = Cat new     // clones Cat, no init called
c name          // => "kitty"
```

### Blocks

Blocks are closures. Parameters go between pipes:

```clay
doubler = { |x| x * 2 }
doubler value(5) println   // 10

// No-arg block
greet = { "hello" println }
greet value
```

### Trailing block syntax

When the last argument to a message is a block, you can write it outside the parentheses:

```clay
arr each { |x| x println }          // instead of arr each({ |x| x println })
arr map { |x| x * 2 }               // instead of arr map({ |x| x * 2 })
arr reduce(0) { |acc, x| acc + x }  // block follows the parenthesized args
(x > 0) ifTrue { "yes" println }    // control flow uses this too
```

### Control flow

Booleans are objects. Control flow is message passing with blocks:

```clay
(x > 0) ifTrue { "positive" println }
(x > 0) ifFalse { "non-positive" println }

// Chaining ifTrue/ifFalse
(x > 0) ifTrue { "positive" } ifFalse { "non-positive" }
```

Blocks capture variables from their enclosing scope (closures). Captured variables are shared by reference — mutations in one closure are visible to all closures that captured the same variable:

```clay
multiplier(factor) {
  { |x| x * factor }   // 'factor' is captured from the method scope
}

// Shared captures
makeCounter() {
  n = 0
  inc = { n = n + 1; n }
  dec = { n = n - 1; n }
  [inc, dec]                // both closures share 'n'
}
```

### Return

`return` exits a method early with a value. Bare `return` returns nil:

```clay
abs(x) {
  (x < 0) ifTrue { return -x }
  x
}
```

> **Note:** `return` inside a block (e.g., `ifTrue { return x }`) currently exits the block, not the enclosing method.

### Break and continue

`break` exits the nearest loop. `continue` skips to the next iteration:

```clay
[1, 2, 3, 4, 5] each { |n|
  (n == 3) ifTrue { break }
  n println   // prints 1, 2
}
```

Works with `times`, `each`, `whileTrue`, and `whileFalse`.

### Imports

Use `import` to include definitions from another Clay file:

```clay
import("math.clay")        // relative to the importing file
import("lib/utils.clay")   // subdirectory paths work too
import("my-brick")         // brick import (see Bricks below)
```

Imports are resolved at parse time. Duplicate imports of the same file are silently skipped. Brick imports (no extension, no path separator) resolve via the `bricks/` directory.

### Namespaced imports

Add `as Name` to scope all imported definitions under a namespace object:

```clay
import("math.clay") as Math

Math PI println           // access values
Math circleArea(2)        // call blocks with args
Math PI = 2.71828         // mutations are local to the namespace
```

The imported code executes normally (setting globals), then a namespace object is created with copies of each top-level assignment. The namespace is independent — mutating it does not affect the originals.

### Destructuring assignment

Unpack arrays into multiple variables in a single statement:

```clay
[a, b, c] = [10, 20, 30]
a println   // 10
b println   // 20

// Works with any expression that returns an array
[x, y] = point toArray
[first, second] = someArray

// Inside blocks and methods too
swap = { |pair| [a, b] = pair; [b, a] }
swap value([1, 2])   // [2, 1]
```

Extra array elements are ignored when there are fewer targets than elements.

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
"Hello, ${name}!" println       // Hello, Clay!
"2 + 3 = ${2 + 3}" println     // 2 + 3 = 5
"${x} + ${y} = ${x + y}" println
```

Expressions are automatically converted via `toString()`.

### Logical operators

```clay
(a && b)     // short-circuit AND
(a || b)     // short-circuit OR
!a           // logical NOT (sends 'not' message)
```

`&&` and `||` short-circuit: the right side is only evaluated if needed. Values are truthy unless they are `nil`, `false`, or `0`.

#### Null-coalescing (`??`)

Returns the left operand if non-nil, otherwise the right:

```clay
x = nil
x ?? "default"         // "default"
a ?? b ?? c            // first non-nil value
```

`??` has lower precedence than all other binary operators.

### While loops

Blocks support `whileTrue` and `whileFalse` for looping:

```clay
i = 0
{ i < 10 } whileTrue {
  i println
  i = i + 1
}
```

### Arrays

```clay
arr = [1, 2, 3]
arr at(0)                     // 1
arr append(4)
arr size                      // 4
arr first                     // 1
arr last                      // 3
arr each { |item| item println }

// Functional methods (trailing block syntax)
arr map { |x| x * 2 }            // [2, 4, 6]
arr filter { |x| x > 1 }         // [2, 3]
arr reduce(0) { |acc, x| acc + x }  // 6
arr find { |x| x > 2 }           // 3

// Utility methods
arr reverse                   // [3, 2, 1]
arr sort                      // [1, 2, 3]
arr join(", ")                // "1, 2, 3"
arr contains(2)               // true
arr indexOf(2)                // 1
arr removeAt(0)               // removes first element
arr flatten                   // flattens nested arrays
arr sum                       // sum of numeric elements
arr min                       // minimum value
arr max                       // maximum value
arr avg                       // average value
arr unique                    // unique elements
arr pop                       // remove + return last
arr shift                     // remove + return first
arr slice(1, 3)               // sub-array [index 1..3)
arr insert(1, "x")            // insert at index
arr setAt(0, "y")             // set element at index

// Equality (element-wise comparison)
[1, 2, 3] == [1, 2, 3]       // true
[1, 2] != [1, 3]             // true
```

### Numbers

Integers are tagged 62-bit values and floats are tagged 32-bit IEEE values — neither allocates. Mixed operations promote to float. Integer overflow currently wraps silently.

```clay
42                            // integer
3.14                          // float
10 / 3                        // 3 (integer division)
10 toFloat / 3                // 3.33333 (float division)
3.14 + 2                      // 5.14 (mixed → float)
3.14 toInt                    // 3 (truncation)
(-5) abs                      // 5
5 times { |i| i println }  // prints 0..4
```

### Strings

Strings support C-style escape sequences: `\n` (newline), `\t` (tab), `\r` (carriage return), `\\` (backslash), `\"` (quote), `\xHH` (hex byte).

```clay
s = "hello world"
s length                            // 11
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
"hello" indexOf("ll")               // 2
"abcabc" lastIndexOf("bc")          // 4
"ab" repeat(3)                      // "ababab"
"42" padLeft(5, "0")                // "00042"
"hi" padRight(8)                    // "hi      "
"line1\nline2" println              // prints two lines
"col1\tcol2" println                // tab-separated
```

### Comments

```clay
// line comment
/* block comment */
```

### Dict (HashMap)

Dict literals use `#{}` syntax — keys are identifiers that become strings:

```clay
d = #{name: "Clay", version: 1}
d get("name")              // "Clay"
empty = #{}
nested = #{outer: #{inner: 1}}
```

The `Dict` prototype also supports programmatic construction:

```clay
d = Dict clone
d set("name", "Clay")
d set("version", 1)

d get("name") println       // "Clay"
d has("name") println       // true
d size println              // 2
d remove("version")

// Iteration
d each { |key, value|
  "${key}: ${value toString}" println
}

// Inspect
d keys println              // [name]
d values println            // [Clay]
d toString println          // {name: Clay}

// Equality (key-value comparison)
d1 = Dict clone; d1 set("a", 1)
d2 = Dict clone; d2 set("a", 1)
d1 == d2                    // true

// Merge, entries, clear
d merge(other)              // merge other dict into d (mutates d)
d entries                   // [[key1, val1], [key2, val2], ...]
d clear                     // remove all entries
```

### Error handling

Clay provides `try`/`catch`/`finally` blocks and `raise` for error handling. Try/catch frames use a dynamic linked list (no fixed depth limit), so deeply nested loops and recursive code work without hitting an artificial ceiling:

```clay
// Catch errors
result = try {
  10 / 0
} catch { |e|
  "Caught: " + e message
}

// Raise custom errors
raise("something went wrong")

// Errors have message() and type() methods
try {
  nil banana
} catch { |e|
  e type println    // "NilError"
  e message println // "message 'banana' sent to nil"
}

// Finally runs whether try succeeds or fails
try {
  f = File open("data.txt")
  // ... use file ...
} catch { |e|
  "Error: " + e message
} finally {
  f close             // always runs
}
```

Error types include `DivisionByZero`, `MethodNotFound`, `NilError`, `TypeError`, and `RuntimeError` (for user-raised strings).

### Stack traces

Errors automatically capture a stack trace showing the call chain:

```clay
boom = { raise("deep error") }
middle = { boom value }

try {
  middle value
} catch { |e|
  e stackTrace println
  // Output:
  //   at example.clay:1 in value()
  //   at example.clay:2 in value()
  //   at example.clay:5 in value()
}
```

`toString()` includes the trace, and unhandled errors print it to stderr.

Objects can override `doesNotUnderstand` for proxy-style dispatch:

```clay
Proxy = Object clone {
  doesNotUnderstand(name, args) {
    "forwarding: " + name
  }
}
```

### Reflection

```clay
Dog = Object clone { _name = "Dog" }
Poodle = Dog clone {}

Poodle parent == Dog          // true (walk the prototype chain)
Poodle hasSlot("_name")       // true
Poodle hasMethod("clone")     // true (inherited from Object)
Poodle methods              // array of method names
Poodle slots                // array of slot names
Poodle type                 // "Dog" (from _name slot)
```

### System

The `System` object provides OS-level operations:

```clay
System platform             // "windows", "macos", or "linux"
System env("PATH")            // environment variable (nil if unset)
System args                 // command-line arguments as array
System clock                // monotonic clock in seconds (float)
System exit(0)                // exit process with code

// Memory diagnostics
System allocCount           // total objects allocated
System deallocCount         // total objects freed
```

### FFI (Foreign Function Interface)

Load and call functions from shared libraries (`.dll`, `.so`, `.dylib`). Plugin functions must match the Clay method signature: `ClayValue fn(ClayObject *self, int argc, ClayValue *argv)`.

```clay
lib = FFI load("./my_plugin.dll")  // or .so / .dylib
result = lib call("my_function", arg1, arg2)
lib close
```

### Native plugins

Plugins are shared libraries that add new prototypes (or methods on existing
ones) written in C. They use only the stable API in
[`src/runtime/clay_plugin.h`](src/runtime/clay_plugin.h) — no Clay internals —
and appear in Clay as ordinary objects:

```c
#include "clay_plugin.h"
#include <math.h>

static const ClayPluginAPI *api;

static ClayPluginValue mathex_sqrt(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("sqrt requires 1 argument", "ArgumentError"); return 0; }
    return api->make_float(sqrtf(api->to_float(argv[0])));
}

static ClayPluginMethod methods[] = { { "sqrt", mathex_sqrt }, { NULL, NULL } };

CLAY_PLUGIN_EXPORT void clay_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "MathEx", methods, NULL, 0 };
    api->register_type(&def);
}
```

```sh
cc -shared -fPIC -I src/runtime mathex.c -o plugins/mathex.so -lm   # Linux / macOS
cl /LD /I src\runtime mathex.c /Fe:plugins\mathex.dll                # Windows
```

```clay
MathEx = System plugin("mathex")   // loads plugins/mathex.{so,dylib,dll}
MathEx sqrt(16) println            // 4
```

`System plugin(name)` looks for `plugins/<name>` in the current directory, then
next to the running script, then next to the `clay` executable; it also accepts
a path. Every plugin in the `plugins/` folder beside the executable is loaded
automatically at startup, so its prototypes are available without the call
(loading the same plugin twice is harmless). The CMake build compiles the
examples in [`examples/plugins/`](examples/plugins/) into `build/plugins/`.

### File operations

```clay
// Read/write
content = File read("data.txt")
lines = File readLines("data.txt")
File write("out.txt", "hello")
File append("out.txt", "\nworld")

// Query
File exists("data.txt")              // true/false
File size("data.txt")                // size in bytes
File isFile("data.txt")              // true
File isDir("src")                    // true

// Modify
File rename("old.txt", "new.txt")
File delete("temp.txt")
File mkdir("output")
File listDir("src")                  // array of filenames
```

### Date and time

```clay
d = Date now
d toString println               // "2026-02-23 14:30:45"
d year                             // 2026
d month                            // 2 (1-12)
d day                              // 23 (1-31)
d hour                             // 14 (0-23)
d minute                           // 30 (0-59)
d second                           // 45 (0-59)
d weekday                          // 1 (0=Sun..6=Sat)
d timestamp                        // Unix epoch seconds

// Create from timestamp
d2 = Date fromTimestamp(1700000000)

// strftime-style formatting
d format("%A, %B %d, %Y") println  // "Sunday, February 23, 2026"
d format("%H:%M") println          // "14:30"
```

### HTTP client

```clay
// GET request
resp = Http get("http://example.com")
resp status println              // 200
resp body println                // HTML content

// HTTPS (Windows always, Unix/macOS with OpenSSL)
resp = Http get("https://example.com")

// POST request
resp = Http post("http://api.example.com/data", "hello")
resp status println
resp body println
```

> **Note:** On Windows, Http uses WinHTTP and supports HTTPS natively. On Unix/macOS, HTTPS requires OpenSSL (`brew install openssl` on macOS, typically pre-installed on Linux). Build with `cmake` and it will auto-detect OpenSSL. Without OpenSSL, only HTTP is supported.

### HTTP server

```clay
// Simple server — handle all requests with a single block
Http serve(8080, { |req|
    method = req method   // "GET", "POST", etc.
    path = req path       // "/api/data"
    query = req query     // "name=clay" (after ?)
    body = req body       // POST body

    Http response(200, "Hello World")
})

// JSON response
Http serve(8080, { |req|
    Http jsonResponse(200, "{\"status\": \"ok\"}")
})
```

For routing, use the `clay-http-server` brick:

```clay
import("clay-stdlib/clay-http-server")

server = HttpServer new(8080)
server get("/", { |req| server html(200, "<h1>Hello!</h1>") })
server get("/api/time", { |req| server json(200, Date now toString) })
server post("/api/echo", { |req| server text(200, req body) })

// URL params with :param patterns
server get("/users/:id", { |req|
    id = req params get("id")
    server json(200, "{\"user\": " + id + "}")
})

// Redirect helper
server post("/login", { |req| server redirect("/dashboard") })
server start
```

### Regular expressions

```clay
// Match first occurrence
Regex match("hello world", "\\w+")           // "hello"

// Test if pattern matches
Regex test("abc123", "\\d+")                 // true

// Replace all matches
Regex replace("a1b2c3", "\\d", "X")          // "aXbXcX"

// Find all matches
Regex find("a1 b2 c3", "\\d+")              // ["1", "2", "3"]

// String convenience methods
"hello world" match("\\w+")                  // "hello"
"abc123" test("[0-9]+")                      // true
```

Supports ERE (Extended Regular Expressions): `.`, `^`, `$`, `*`, `+`, `?`, `[...]`, `[^...]`, `\d`, `\w`, `\s`, `(...)`, `|`, `\\`.

### Fibers (coroutines)

```clay
// Create a fiber from a block
f = Fiber new {
    Fiber yield(1)
    Fiber yield(2)
    3
}

f resume     // 1
f resume     // 2
f resume     // 3 (final value)
f isDone     // true

// Generator pattern — yield from inside loops
counter = Fiber new {
    i = 0
    { i < 5 } whileTrue {
        Fiber yield(i)
        i = i + 1
    }
}
counter resume   // 0
counter resume   // 1
counter resume   // 2

// Yield from array iteration
items = Fiber new {
    [10, 20, 30] each { |x| Fiber yield(x) }
}
items resume     // 10
items resume     // 20

// Two independent fibers
a = Fiber new { Fiber yield(10); 20 }
b = Fiber new { Fiber yield(30); 40 }
a resume     // 10
b resume     // 30
a resume     // 20
b resume     // 40
```

### Vec3

A built-in 3D vector type optimized for math-heavy code (ray tracing, physics, etc.).

```clay
// Create vectors
v = Vec3 new(1.0, 2.0, 3.0)
v x   // 1.0
v y   // 2.0
v z   // 3.0

// Arithmetic (scalar and vector)
a = Vec3 new(1, 2, 3)
b = Vec3 new(4, 5, 6)
a + b              // Vec3(5, 7, 9)
a - b              // Vec3(-3, -3, -3)
a * 2.0            // Vec3(2, 4, 6)
a / 2.0            // Vec3(0.5, 1, 1.5)
a mul(b)           // Vec3(4, 10, 18)  — component-wise

// Geometry
a dot(b)           // 32.0
a cross(b)         // Vec3(-3, 6, -3)
a length           // 3.7416...
a lengthSquared    // 14
a normalize        // unit vector
a negated          // Vec3(-1, -2, -3)
a nearZero         // false

// Reflection (for ray tracing)
incoming = Vec3 new(1, -1, 0) normalize
normal = Vec3 new(0, 1, 0)
incoming reflect(normal)   // reflected direction
```

### Threads

OS-level threads for CPU-bound parallelism. Requires building with `-DCLAY_THREAD_SAFE=ON`.

```clay
// Check if threading is available at runtime
Thread available   // true (thread-safe build) or false

// Spawn a thread — runs a block on a new OS thread
t = Thread spawn { 2 + 3 }
t await            // 5  — blocks until thread completes
t done             // true

// parallelMap — parallel map over an array
// Auto-detects CPU count, chunks work across threads
results = [1, 2, 3, 4, 5] parallelMap { |x| x * x }
results   // [1, 4, 9, 16, 25]

// Works with complex tasks — each element processed on a worker thread
scanlines parallelMap { |args|
    // render a scanline (runs in parallel)
    renderScanline value(args)
}

// Falls back to sequential map when threading is unavailable
```

Building with thread support:

```sh
cmake -B build -DCLAY_THREAD_SAFE=ON
cmake --build build --config Release
```

### Graphics

SDL2-based graphics for windows, drawing, sprites, and input. Automatically enabled when SDL2 is found by CMake (install via `vcpkg install sdl2`).

```clay
// Open a window
Graphics window(800, 600, "My App")

// Drawing primitives
Graphics clear
Graphics color(255, 0, 0)           // set draw color (RGB)
Graphics drawLine(0, 0, 100, 100)
Graphics drawRect(50, 50, 200, 150)
Graphics fillRect(50, 50, 200, 150)
Graphics drawCircle(400, 300, 50)
Graphics fillCircle(400, 300, 50)
Graphics setPixel(10, 20, 255, 128, 0)   // x, y, r, g, b
Graphics refresh                          // present to screen

// Text
Graphics fontSize(24)
Graphics drawText(10, 10, "Hello!")

// Images (PPM format)
img = Graphics loadPPM("image.ppm")
Graphics drawImage(img, 0, 0)
Graphics drawImageEx(img, 0, 0, 2.0)   // with scale

// Sprites (sub-image regions from a source image)
sprite = Graphics createSprite(img, 0, 0, 32, 32)   // x, y, w, h
Graphics drawSprite(sprite, 100, 100)
Graphics drawSpriteEx(sprite, 100, 100, 2.0, 0, false, false)  // scale, angle, flipX, flipY

// Offscreen buffers
buf = Graphics createBuffer(320, 240)
Graphics bufferPixel(buf, 10, 10, 255, 0, 0)
Graphics drawBuffer(buf, 0, 0)

// Event handling
evt = Graphics pollEvent       // non-blocking, returns Dict or nil
evt = Graphics waitEvent       // blocking
(evt != nil) ifTrue {
    evt get("type")            // "quit", "keydown", "keyup", "mousedown", "mousemove", etc.
    evt get("key")             // key name (for key events)
    evt get("x")               // mouse x (for mouse events)
}

// Input queries
Graphics isKeyDown("Space")   // true/false
Graphics mouseX               // current mouse x
Graphics mouseY               // current mouse y
Graphics mouseDown             // true if any mouse button pressed

// Timing
Graphics ticks                 // milliseconds since SDL init
Graphics delay(16)             // sleep ~16ms (for frame limiting)
Graphics fps                   // frames per second

// Window management
Graphics width                 // window width
Graphics height                // window height
Graphics resize(1024, 768)
Graphics fullscreen
Graphics windowed
Graphics close
```

### Database (SQLite)

```clay
// Open (or create) a database
db = Db open("app.db")

// Create a table
db exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)")

// Insert data
db exec("INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')")
db exec("INSERT INTO users (name, email) VALUES ('Bob', 'bob@example.com')")

// Query — returns an Array of Dicts
rows = db query("SELECT id, name, email FROM users")
rows each { |row|
    (row get("name") + ": " + row get("email")) println
}
// Alice: alice@example.com
// Bob: bob@example.com

// Last insert ID
db exec("INSERT INTO users (name, email) VALUES ('Carol', 'carol@example.com')")
db lastInsertId   // 3

// Escape strings for safe SQL (doubles single quotes)
db escape("O'Brien")   // "O''Brien"

// Float columns are returned as Clay floats
db exec("CREATE TABLE prices (id INTEGER PRIMARY KEY, amount REAL)")
db exec("INSERT INTO prices (amount) VALUES (9.99)")
db query("SELECT amount FROM prices") at(0) get("amount")   // 9.99

// Close when done
db close
```

## Core Objects

| Prototype | Methods |
|-----------|---------|
| Object | `clone()`, `new(args...)`, `toString()`, `println()`, `type()`, `methods()`, `slots()`, `inspect()`, `parent()`, `hasSlot(name)`, `getSlot(name)`, `setSlot(name, val)`, `hasMethod(name)`, `setMethod(name, block)`, `doesNotUnderstand(name, args)` |
| Number | `+`, `-`, `*`, `/`, `%`, `<`, `>`, `<=`, `>=`, `==`, `!=`, `abs()`, `toFloat()`, `toInt()`, `toString()`, `println()`, `times(block)` |
| String | `+`, `==`, `length()`, `at(i)`, `chr(code)`, `contains(sub)`, `slice(start, end)`, `split(delim)`, `replace(old, new)`, `toInt()`, `toFloat()`, `trim()`, `upper()`, `lower()`, `startsWith(s)`, `endsWith(s)`, `reverse()`, `indexOf(sub)`, `lastIndexOf(sub)`, `repeat(n)`, `padLeft(n, ch)`, `padRight(n, ch)`, `match(pattern)`, `test(pattern)`, `print()`, `println()`, `toString()` |
| Array | `at(i)`, `setAt(i, val)`, `append(val)`, `push(val)`, `pop()`, `shift()`, `insert(i, val)`, `size()`, `length()`, `each(block)`, `map(block)`, `parallelMap(block)`, `filter(block)`, `reduce(init, block)`, `find(block)`, `sort()`, `reverse()`, `first()`, `last()`, `join(sep)`, `contains(val)`, `indexOf(val)`, `removeAt(i)`, `slice(start, end)`, `flatten()`, `sum()`, `min()`, `max()`, `avg()`, `unique()`, `==`, `!=`, `toString()` |
| Block | `value(args...)`, `whileTrue { block }`, `whileFalse { block }` |
| Boolean | `ifTrue { block }`, `ifFalse { block }`, `not()`, `toString()` |
| Error | `message()`, `type()`, `line()`, `file()`, `stackTrace()`, `toString()` |
| Dict | `set(key, val)`, `get(key)`, `has(key)`, `remove(key)`, `size()`, `keys()`, `values()`, `each(block)`, `==`, `!=`, `toString()` |
| Math | `abs(n)`, `sqrt(n)`, `pow(b,e)`, `min(a,b)`, `max(a,b)`, `floor(n)`, `ceil(n)`, `round(n)`, `sin(n)`, `cos(n)`, `random()`, `pi`, `e` |
| Console | `print(args...)`, `println(args...)`, `readLine()`, `prompt(msg)` |
| File | `read(path)`, `readLines(path)`, `write(path, content)`, `append(path, content)`, `exists(path)`, `delete(path)`, `rename(old, new)`, `size(path)`, `isDir(path)`, `isFile(path)`, `mkdir(path)`, `listDir(path)` |
| System | `exit(code)`, `env(name)`, `args()`, `platform()`, `clock()`, `allocCount()`, `deallocCount()` |
| FFI | `load(path)`, `call(funcName, args...)`, `close()` |
| Date | `now()`, `fromTimestamp(ts)`, `year()`, `month()`, `day()`, `hour()`, `minute()`, `second()`, `weekday()`, `timestamp()`, `format(pattern)`, `toString()` |
| Http | `get(url)`, `post(url, body)`, `serve(port, handler)`, `response(status, body)`, `jsonResponse(status, body)`, `parseForm(str)`, `status()`, `body()`, `method()`, `path()`, `query()`, `params()` |
| Regex | `match(str, pattern)`, `test(str, pattern)`, `replace(str, pattern, sub)`, `find(str, pattern)` |
| Fiber | `new(block)`, `resume()`, `isDone()`, `yield(value)` |
| Db | `open(path)`, `exec(sql)`, `query(sql)`, `close()`, `escape(str)`, `lastInsertId()` |
| Range | `each(block)`, `map(block)`, `filter(block)`, `contains(val)`, `toArray()`, `size()`, `step(n)`, `reverse()`, `start()`, `end()`, `toString()` |
| Vec3 | `new(x,y,z)`, `x`, `y`, `z`, `+`, `-`, `*`, `/`, `dot(v)`, `cross(v)`, `length`, `lengthSquared`, `normalize`, `mul(v)`, `reflect(n)`, `nearZero`, `negated`, `==`, `!=`, `toString()` |
| Thread | `spawn(block)`, `await()`, `done()`, `available` |
| Graphics | `window(w,h,title)`, `close`, `clear`, `color(r,g,b)`, `setPixel(x,y,r,g,b)`, `drawLine(x1,y1,x2,y2)`, `drawRect(x,y,w,h)`, `fillRect(x,y,w,h)`, `drawCircle(x,y,r)`, `fillCircle(x,y,r)`, `drawText(x,y,text)`, `fontSize(n)`, `loadPPM(path)`, `drawImage(img,x,y)`, `drawImageEx(img,x,y,scale)`, `createSprite(img,x,y,w,h)`, `drawSprite(s,x,y)`, `createBuffer(w,h)`, `bufferPixel(buf,x,y,r,g,b)`, `drawBuffer(buf,x,y)`, `pollEvent`, `waitEvent`, `isKeyDown(key)`, `mouseX`, `mouseY`, `mouseDown`, `ticks`, `delay(ms)`, `fps`, `refresh`, `resize(w,h)`, `fullscreen`, `windowed`, `width`, `height` |

## Generated C

Clay generates readable C. This Clay code:

```clay
p = Point clone
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

**VM mode** (`clay file.clay`): compiles to bytecode and executes instantly in the embedded bytecode VM. The VM bridges to the same Clay runtime via native functions, so all core objects and methods work identically.

**C compile mode** (`clay -c file.clay`): generates human-readable C with `#line` directives, invokes the system C compiler, and links the Clay runtime as a static library.

**VM REPL mode** (`clay` or `clay --vm-repl`): interactive REPL that compiles expressions to bytecode and evaluates them instantly.

**DAP mode** (`clay --dap`): runs as a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/) server for VS Code and other DAP-compatible editors. Supports breakpoints, stepping, variable inspection, and output capture.

**Method cache**: message dispatch caches resolved methods per (receiver-prototype, selector) pair, avoiding repeated prototype-chain walks. The cache is automatically invalidated when methods are added or changed.

## Bricks (Package Manager)

Bricks is Clay's built-in package manager. Packages ("bricks") are Git repositories with a `brick.json` manifest. Any Git URL works — no central registry needed.

### Quick start

```sh
clay bricks init                              # create brick.json
clay bricks add github.com/user/some-lib      # add + install a dependency
clay bricks install                           # install all deps from brick.json
clay bricks update                            # update all deps to latest
clay bricks list                              # show project info
clay bricks remove some-lib                   # remove a dependency
```

### brick.json

```json
{
  "name": "my-project",
  "version": "0.1.0",
  "description": "A Clay project",
  "main": "src/main.clay",
  "deps": {
    "some-lib": "github.com/user/some-lib@v1.0"
  }
}
```

Dependencies are pinned to a Git tag, branch, or commit via `@ref`. URLs are automatically prefixed with `https://` if no protocol is specified.

### Using bricks

Import a brick by name (no `.clay` extension, no path separator):

```clay
import("some-lib")    // resolves to bricks/some-lib/ using brick.json main
import("utils.clay")  // regular file import (unchanged)
```

Brick resolution: reads `bricks/<name>/brick.json` to find the `main` entry point. Falls back to `bricks/<name>/src/main.clay` if no manifest exists.

### Monorepo bricks

A single Git repository can contain multiple bricks (a "monorepo"). Import sub-bricks using slash-separated paths:

```clay
import("clay-stdlib/clay-collections")   // sub-brick inside clay-stdlib
import("clay-stdlib/clay-json")          // another sub-brick
```

In `brick.json`, declare monorepo dependencies with `#sub-brick` syntax:

```json
{
  "deps": {
    "clay-stdlib/clay-collections": "github.com/mseminatore/clay-stdlib#clay-collections"
  }
}
```

Sub-bricks resolve to `bricks/<repo>/<sub-brick>/brick.json`.

### Publishing a brick

Any Git repository with a `brick.json` is a valid brick. To publish:
1. Add a `brick.json` to your repo (`clay bricks init`)
2. Push to GitHub (or any Git host)
3. Tag releases with semver (`git tag v1.0.0`)
4. Others install with `clay bricks add github.com/you/your-brick@v1.0.0`

### Local layout

```
my-project/
  brick.json              # project manifest
  src/main.clay            # your code
  bricks/                  # dependencies (gitignore this)
    some-lib/
      brick.json
      src/main.clay
```

The `bricks/` directory should be gitignored — it's recreated by `clay bricks install`.

### Available bricks

The standard library is available at:

| Brick | Description | Repo |
|-------|-------------|------|
| **[clay-stdlib](https://github.com/mseminatore/clay-stdlib)** | Standard library monorepo with `Assert`, `Time`, `MathUtil`, and all bricks below | `github.com/mseminatore/clay-stdlib` |

**clay-stdlib** contains these sub-bricks:

| Sub-brick | Description |
|-----------|-------------|
| **clay-args** | `ArgParser` CLI parsing: `flag`, `option`, `parse`, `usage` |
| **clay-base64** | `Base64` encoding/decoding: `encode`, `decode` with standard alphabet and padding |
| **clay-cache** | `Cache` in-memory LRU/TTL key-value store: `get`, `set`, `has`, `remove`, `clear` |
| **clay-cli** | ANSI terminal colors and styles for CLI output |
| **clay-collections** | Set, Stack, Queue, Range data structures |
| **clay-config** | `Config` INI-style parser: `parse`/`parseFile` into Dict of Dicts, `get` with defaults, `merge` |
| **clay-csv** | `Csv` parse/stringify: `parse` (array of arrays), `parseWithHeader` (array of dicts), `stringify` for CSV generation |
| **clay-enum** | `Enum` named constants with integer value mapping |
| **clay-env** | `Env` environment management: `load` `.env` files, `get` with defaults, `require`, `has` |
| **clay-event** | `Emitter` pub/sub pattern: `on`, `off`, `emit`, `once`, `removeAll`, `listenerCount` |
| **clay-format** | `Fmt` string utilities: `padLeft`, `padRight`, `center`, `repeat`, `capitalize`, `titleCase`, `truncate`, `wrap`, `replaceAll` |
| **clay-functional** | `Fn` utilities: `map`, `filter`, `reduce`, `compose`, `pipe`, `any`, `all`, `find`, `zip`, `flatten`, `sum`, `product` |
| **clay-http-server** | `HttpServer` routing: `get`/`post`/`put`/`delete` routes with `:param` patterns, `text`/`html`/`json`/`redirect` response helpers |
| **clay-iter** | `Iter` lazy pipelines: `from`/`range` sources, `map`/`filter`/`take`/`skip` transforms, `toArray`/`reduce`/`first`/`count`/`each` consumers |
| **clay-json** | `Json` parse/stringify: recursive-descent parser for JSON strings, `stringify`/`prettyPrint` for serialization |
| **clay-log** | `Log` structured logging: `debug`/`info`/`warn`/`error` levels with timestamps, prefixes, and level filtering |
| **clay-markdown** | `Markdown` converter: headings, code blocks, tables, lists, blockquotes to HTML |
| **clay-match** | `Match` pattern matching: chainable `when`/`whenType`/`otherwise` with value and predicate matching |
| **clay-orm** | `Model` ActiveRecord-style ORM: `define`/`create`/`find`/`all`/`where`/`count`/`update`/`delete` over SQLite |
| **clay-path** | `Path` manipulation: `join`, `dirname`, `basename`, `extension`, `stem` |
| **clay-queue** | `Queue` SQLite-backed persistent job queue with status tracking |
| **clay-router** | `Router` URL pattern matching: `add`/`match` with `:param` capture, multi-segment patterns |
| **clay-template** | `Template` engine: `render` with `{{key}}` placeholders, `renderEach` for lists |
| **clay-test** | Testing framework: `describe`/`it` blocks, colored output, assertions (`assertEqual`, `assertTrue`, `assertFalse`, `assertRaises`) |
| **clay-url** | `Url` parse/build URLs, `encodeComponent`/`decodeComponent`, `parseQuery` for query strings |
| **clay-uuid** | `Uuid` v4 generation per RFC 4122 |
| **clay-validate** | Schema validation for Dicts with field rules (required, optional, min, max) |

Install the standard library (includes all sub-bricks):

```sh
clay bricks add github.com/mseminatore/clay-stdlib
```

Then import individual sub-bricks:

```clay
import("clay-stdlib/clay-collections")
import("clay-stdlib/clay-json")
```

Or import everything:

```clay
import("clay-stdlib")
```

## Dependencies

Included under `deps/`:

- [mseminatore/hash](https://github.com/mseminatore/hash) — hash table for object slots and methods
- [mseminatore/testy](https://github.com/mseminatore/testy) — test framework for runtime unit tests
- [jcwangxp/Crossline](https://github.com/jcwangxp/Crossline) — cross-platform readline for the REPL (arrow keys, tab completion, history on all platforms)
- [SQLite](https://sqlite.org/) — embedded SQL database engine for the `Db` prototype
- [cJSON](https://github.com/DaveGamble/cJSON) — lightweight JSON parser for the DAP debug adapter
- [mseminatore/vmgen](https://github.com/mseminatore/vmgen) — generates the bytecode VM in `src/vm/generated/` from `src/vm/clay.yaml` (only needed when changing the VM: `cmake --build build --target regen_vm`)

## Editor Support

### VS Code

**Syntax highlighting:** A TextMate grammar extension is included at `editors/vscode/clay-lang/`. To install:

```sh
# macOS / Linux — symlink into your extensions folder
ln -s "$(pwd)/editors/vscode/clay-lang" ~/.vscode/extensions/clay-lang
```

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
| **Run current .clay file** | Launch: "Run: Current .clay File" |
| Debug test runner | Launch: "Debug: Test Runner" |
| Debug VM REPL | Launch: "Debug: VM REPL" |
| Compile to native + debug | Launch: "Debug: Compiled .clay Program" |
| Compile to C only | Task: "Compile .clay to C Only" |
| **Debug Clay program** | Launch: "Debug Clay Program" (uses DAP) |

The Clay extension includes a **Debug Adapter** that supports source-level debugging of `.clay` files. Set breakpoints, step through code, and inspect variables — all within VS Code. The adapter communicates with `clay --dap` over the Debug Adapter Protocol.

In compiled mode, `#line` directives map generated C back to `.clay` source lines, so the Visual Studio debugger steps through Clay code directly.

## License

Clay is released under the [MIT License](LICENSE). Bundled and optional
third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
