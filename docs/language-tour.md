# Language tour

A walk through Clay's syntax and object model. Everything here runs in the VM
(`clay file.clay`) and natively (`clay --compile file.clay`). The built-in
prototypes are covered in [Core objects](core-objects.md).

## Everything is an object

Integers, strings, booleans, arrays, blocks — all objects. All operations are message sends.

```clay
42 println              // send 'println' to an integer
"hello" length          // send 'length' to a string
```

## Variables

A variable comes into existence when it's first assigned: `count = 0`. One
assigned at the top level of a program is global — visible everywhere,
including in code written above the assignment. One assigned inside a block
belongs to that block.

Reading a variable that is never defined is an error, reported before the
program runs (in the VM and with `--compile` alike), so a typo can't silently
become `nil`:

```clay norun
p = Pointt clone    // error: undefined variable 'Pointt'
```

Inside a method a bare name can also be one of `self`'s slots (see
[Implicit self](#implicit-self-in-methods)), so names there aren't checked.

## Message passing

Messages use IO-style syntax: `receiver message(args)`. Parentheses are optional for zero-argument messages.

```clay
arr = [10, 20, 30]
"hello" println          // no parens (preferred for unary)
"hello" println()        // parens also work
"hello" length           // returns 5
arr at(0)                // message with one arg: 10
arr each { |x| x println }  // message with trailing block
```

## Binary operators

Binary operators (`+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`) are messages sent to the left operand.

```clay
3 + 4         // sends '+' to 3 with arg 4
10 > 5        // sends '>' to 10, returns true
```

Standard math precedence applies.

**Messages bind tighter than operators.** `x size + 1` is `(x size) + 1`, which is
usually what you want — but it also means a message after an operator goes to
the operand next to it, not to the whole comparison:

```clay
x = 5
(x == 5) ifTrue { "five" println }   // parenthesize the condition
// x == 5 ifTrue { ... } would send ifTrue to 5, not to the comparison:
// "integer does not understand 'ifTrue' (a message binds tighter ...)"
```

The same goes for loops: `{ i < 3 } whileTrue { ... }` puts the condition in a
block, which is what `whileTrue` is sent to.

## Operator overloading

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

## Prototypes and cloning

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

Either brace style works: the `{` of a clone body or a method body may also
start the next line.

```clay
Counter = Object clone
{
  count = 0

  increment()
  {
    count = count + 1
  }
}
Counter increment
Counter count println   // 1
```

For a clone body on the next line, the `{` must be alone on its line. A `{`
with more after it on the line starts the next statement instead, as a block:

```clay
Shape = Object clone
{
    sides = 0
}
n = 0
s = Shape clone
{ n < 3 } whileTrue { n = n + 1 }   // a loop, not the clone's body
n println                            // 3
```

Inside methods, bare identifiers like `x` resolve to slots on `self` — both for reads and writes: if `self` has a slot `x`, then `x = val` writes to it (Io-style `updateSlot` semantics). Names that aren't slots create method-local variables. Use `self x = val` when a parameter shadows a slot name, as in the `init` pattern below. The exact rules are in [Implicit self](#implicit-self-in-methods).

## Super

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

## Slot access

Read a slot by sending its name as a zero-argument message, and write it with
assignment syntax on the receiver:

```clay
Point = Object clone {
  x = 0
  y = 0
}
p = Point clone
p x println     // 0 (reads the 'x' slot, inherited from Point)
p x = 42        // sets slot 'x' on p
p x println     // 42
Point x println // 0 — writes are always local to the receiver
```

Slot reads walk the prototype chain — if the object doesn't have the slot, its parent is checked, and so on.

Slots and methods share one namespace, and the nearest definition along the
chain wins: a slot hides a method of the same name inherited from further up
(on the same object, a method wins over a slot). So a slot may be named like
one of `Object`'s methods:

```clay
Node = Object clone {
  parent = nil      // hides Object's `parent` method for nodes
  type = "node"     // hides Object's `type`
}
n = Node clone
n parent println    // nil
n type println      // node
(Object clone parent == Object) println   // true — the method, elsewhere
```

## Implicit self in methods

Inside a method body, bare identifiers that aren't local variables automatically resolve to `self` slots. This means you can write:

```clay
Point = Object clone {
  x = 0
  y = 0
  toString() { "(" + x toString + ", " + y toString + ")" }
}
```

The lookup order is: local variable or parameter → captured variable → a slot declared in the enclosing `clone` body → top-level variable (wherever it is assigned in the file) → `self` slot or method. So a slot declared in the object's own body always beats a top-level variable of the same name, which keeps a brick's objects working whatever names the script using them chooses:

```clay
Config = Object clone {
  path = "/etc/app.conf"
  show() { path }          // the slot, not the top-level `path` below
}
path = "./local.conf"
Config show println        // /etc/app.conf
```

A slot that's only inherited from a prototype, not declared in this clone body, isn't known when the method is compiled, so there a top-level variable of the same name still wins — write `self path` to be explicit. Slot writes also resolve implicitly (Io-style `updateSlot` semantics): `x = val` in a method writes the slot `x` if the name is declared in the enclosing `clone` block **or** if `self` (or its prototype chain) already has a slot `x` when the method is called — for example one created by a constructor. Otherwise `x` is a new local variable. Assignment in a method, or in a block inside one, never changes a top-level variable (top-level blocks do share the program's variables):

```clay
Point = Object clone {
  x = 0
  y = 0
  moveTo(nx, ny) {
    x = nx       // writes to slot — no self needed
    y = ny       // same here
  }
}

Counter = Object clone {
  new() {
    c = self clone
    c count = 0     // slot created at runtime, not declared in the clone block
    c
  }
  bump() {
    count = count + 1   // still a slot write: this Counter has a `count` slot
    tmp = 1             // no `tmp` slot, so this is a local
    self
  }
}

c = Counter new
c bump bump
c count println       // 2
```

The choice is made once per call, when the method (or a block inside it) starts.

Use explicit `self` when a parameter name shadows a slot name:

```clay
Point = Object clone {
  x = 0
  y = 0
  init(x, y) {
    self x = x   // self needed because parameter 'x' shadows slot 'x'
    self y = y
  }
}
(Point new(3, 4) x) println   // 3
```

## Dynamic Methods

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
Dog = Object clone { name = "Rex" }
Dog speak = { self name + " says Woof!" }

block = Dog getSlot("speak")   // the block itself, not its result
(block type) println           // Block
```

## Constructors

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

### Default parameters

Methods can have default values for trailing parameters:

```clay
Greeter = Object clone {
  greet(name, greeting = "Hello") {
    "${greeting}, ${name}!" println
  }
}
Greeter greet("world")             // Hello, world!
Greeter greet("world", "Hi")       // Hi, world!
```

`new` clones the prototype and calls `init(args...)` on the clone if an `init` method exists. If there is no `init`, `new` just clones:

```clay
Cat = Object clone { name = "kitty" }
c = Cat new     // clones Cat, no init called
c name          // => "kitty"
```

## Blocks

Blocks are closures. Parameters go between pipes:

```clay
doubler = { |x| x * 2 }
doubler value(5) println   // 10

// No-arg block
greet = { "hello" println }
greet value
```

## Trailing block syntax

When the last argument to a message is a block, you can write it outside the parentheses:

```clay
arr = [1, 2, 3]
x = 5
arr each { |x| x println }          // instead of arr each({ |x| x println })
arr map { |x| x * 2 }               // instead of arr map({ |x| x * 2 })
arr reduce(0) { |acc, x| acc + x }  // block follows the parenthesized args
(x > 0) ifTrue { "yes" println }    // control flow uses this too
```

## Control flow

Booleans are objects. Control flow is message passing with blocks:

```clay
x = 5
(x > 0) ifTrue { "positive" println }
(x > 0) ifFalse { "non-positive" println }

// Chaining ifTrue/ifFalse — the whole expression has a value
sign = (x > 0) ifTrue { "positive" } ifFalse { "non-positive" }
sign println           // positive
```

Blocks capture variables from their enclosing scope (closures). Captured variables are shared by reference — mutations in one closure are visible to all closures that captured the same variable:

```clay
Factory = Object clone {
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
}

triple = Factory multiplier(3)
(triple value(5)) println       // 15

pair = Factory makeCounter
inc = pair at(0)
dec = pair at(1)
inc value
inc value
(dec value) println             // 1 — dec sees inc's changes
```

## Return

`return` exits a method early with a value. Bare `return` returns nil:

```clay
Shapes = Object clone {
  area(w, h) {
    (w <= 0 || h <= 0) ifTrue { return 0 }
    return w * h
    "never reached"
  }
}
(Shapes area(3, 4)) println     // 12
(Shapes area(0 - 1, 4)) println // 0
```

`return` inside a block returns from the *method the block was written in*,
however deeply the block is nested, so it can end a loop early (`finally`
blocks still run on the way out):

```clay
Finder = Object clone {
  firstOver(items, limit) {
    items each { |x| (x > limit) ifTrue { return x } }
    nil
  }
}
(Finder firstOver([3, 8, 12, 20], 10)) println   // 12
(Finder firstOver([1, 2], 10)) println           // nil
```

A block that outlives its method can't return from it any more; calling its
`return` then raises a `RuntimeError`. Outside methods there is nothing to
return from, so `return` only ends the innermost block it is written in —
inside a top-level block, `(x > 1) ifTrue { return "big" }` ends just the
`ifTrue` block. Put such logic in a method.

## Break and continue

`break` exits the nearest loop. `continue` skips to the next iteration:

```clay
[1, 2, 3, 4, 5] each { |n|
  (n == 3) ifTrue { break }
  n println   // prints 1, 2
}
```

Works with `times`, `each`, `whileTrue`, and `whileFalse`.

## Imports

Use `import` to include definitions from another Clay file:

```clay norun
import("math.clay")        // relative to the importing file
import("lib/utils.clay")   // subdirectory paths work too
import("my-brick")         // brick import (see Bricks)
```

Imports are resolved at parse time. Duplicate imports of the same file are silently skipped. Brick imports (no extension, no path separator) resolve via the `bricks/` directory.

## Namespaced imports

Add `as Name` to scope all imported definitions under a namespace object:

```clay norun
import("math.clay") as Math

Math PI println           // access values
Math circleArea(2)        // call blocks with args
Math PI = 2.71828         // mutations are local to the namespace
```

The imported code executes normally (setting globals), then a namespace object is created with copies of each top-level assignment. The namespace is independent — mutating it does not affect the originals.

## Destructuring assignment

Unpack arrays into multiple variables in a single statement:

```clay
[a, b, c] = [10, 20, 30]
a println   // 10
b println   // 20

// Works with any expression that returns an array
point = [3, 4]
[x, y] = point
"x=${x} y=${y}" println     // x=3 y=4
[first, second] = "a,b,c" split(",")

// Inside blocks and methods too
swap = { |pair| [a, b] = pair; [b, a] }
swap value([1, 2])   // [2, 1]
```

Extra array elements are ignored when there are fewer targets than elements.

## Unary minus

Negative numbers work naturally:

```clay
a = 2
b = 3
x = -5
y = -(a + b)
y println          // -5
```

## String interpolation

Use `${}` inside strings to embed expressions:

```clay
name = "Clay"
"Hello, ${name}!" println       // Hello, Clay!
"2 + 3 = ${2 + 3}" println     // 2 + 3 = 5
x = 4
y = 6
"${x} + ${y} = ${x + y}" println  // 4 + 6 = 10
```

Expressions are automatically converted via `toString()`.

## Logical operators

```clay
a = true
b = false
(a && b) println     // false — short-circuit AND
(a || b) println     // true  — short-circuit OR
(!a) println         // false — logical NOT (sends 'not')
```

`&&` and `||` short-circuit: the right side is only evaluated if needed. Values are truthy unless they are `nil`, `false`, or `0`.

### Null-coalescing (`??`)

Returns the left operand if non-nil, otherwise the right:

```clay
x = nil
(x ?? "default") println   // default
a = nil
b = nil
c = 3
(a ?? b ?? c) println      // 3 — first non-nil value
```

`??` has lower precedence than all other binary operators.

## While loops

Blocks support `whileTrue` and `whileFalse` for looping:

```clay
i = 0
{ i < 10 } whileTrue {
  i println
  i = i + 1
}
```

## Arrays

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

## Numbers

Integers and floats are stored directly in the value (no allocation). Mixed operations promote to float.

- **Integers** are 62-bit signed: from −2⁶¹ to 2⁶¹ − 1 (±2,305,843,009,213,693,951). A result outside that range raises `OverflowError` rather than wrapping; convert with `toFloat` first if you need larger magnitudes. Integer literals beyond the range are a compile error.
- **Floats** are 32-bit IEEE 754 *single* precision: about 7 significant decimal digits, magnitudes up to ~3.4 × 10³⁸. Integers above 2²⁴ (16,777,216) are not all exactly representable, and small decimal fractions are rounded (`1000000.0 + 0.1` is `1000000.125` internally). Floats print as the shortest text that reads back as the same value — `0.1`, `1000000.1`, `0.33333334` — in plain notation below 10⁹ and exponent notation beyond (`1e+20`). Float arithmetic follows IEEE 754: dividing a float by zero gives `inf` or `nan`, while integer division (or `%`) by zero raises `DivisionByZero`.

```clay
42                            // integer
3.14                          // float
10 / 3                        // 3 (integer division)
10 toFloat / 3                // 3.3333333 (float division)
3.14 + 2                      // 5.1400003 (mixed → float; 32-bit rounding shows)
3.14 toInt                    // 3 (truncation)
(-5) abs                      // 5
5 times { |i| i println }  // prints 0..4
```

## Strings

Strings support C-style escape sequences: `\n` (newline), `\t` (tab), `\r` (carriage return), `\\` (backslash), `\"` (quote), `\xHH` (hex byte).

```clay
s = "hello world"
s length                            // 11
s at(0)                             // "h"
s contains("world")                 // true
s slice(0, 5)                       // "hello"
s split(" ")                        // ["hello", "world"]
s replace("world", "Clay")          // "hello Clay"
try { s toInt } catch { |e| e type() }   // TypeError — not a number
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

## Comments

```clay
// line comment
/* block comment */
```

## Dict (HashMap)

A Dict keeps its entries in the order their keys were first added: `keys`,
`values`, `each`, `entries` and `toString` all follow it. Updating a key keeps
its place; removing it and adding it again moves it to the end.

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
other = #{"version": 2}
d merge(other)              // merge other dict into d (mutates d)
d entries                   // [[key1, val1], [key2, val2], ...]
d clear                     // remove all entries
```

## Error handling

Clay provides `try`/`catch`/`finally` blocks and `raise` for error handling. Try/catch frames use a dynamic linked list (no fixed depth limit), so deeply nested loops and recursive code work without hitting an artificial ceiling:

```clay
// Catch errors — try/catch is an expression
result = try {
  10 / 0
} catch { |e|
  "Caught: " + e message
}
result println             // Caught: division by zero

// Raise custom errors
r = try { raise("something went wrong") } catch { |e| e message }
r println                  // something went wrong

// Errors have message() and type() methods
try {
  nil banana
} catch { |e|
  e type println    // NilError
  e message println // message 'banana' sent to nil
}

// Finally runs whether try succeeds or fails
try {
  File read("no-such-file.txt")
} catch { |e|
  ("Error: " + e message) println
} finally {
  "cleanup always runs" println
}
```

`finally` runs on every way out of a `try`: after the try block succeeds, after
`catch` handles an error, and before an error leaves the `try` — one with no
`catch` (`try { ... } finally { ... }` cleans up and lets the error propagate),
or one raised by the `catch` block itself. `break`, `continue` and `return` pass
through it the same way. An error raised inside `finally` replaces the one in
flight.

Error types include `DivisionByZero`, `OverflowError` (integer results outside the 62-bit range), `MethodNotFound`, `NilError`, `TypeError` (an argument of the wrong type, e.g. `Math sqrt("x")`), `ArgumentError` (a missing or invalid argument, or too many arguments to a method), and `RuntimeError` (for user-raised strings).

Methods written in Clay raise `ArgumentError` when called with more arguments than they declare (fewer is fine — missing parameters are `nil` or take their defaults). Blocks accept extra arguments, so `each { |x| ... }` works on a Dict too.

## Stack traces

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
(Proxy anything) println    // forwarding: anything
```

## Reflection

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

---

[← Guide index](README.md)
