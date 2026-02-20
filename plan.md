# Plan: Clay Programming Language

## Overview
**Clay** is a prototype-based, pure object-oriented language that compiles to human-readable C. Everything is an object. Objects communicate via message passing (IO-style syntax). New objects are created by cloning existing ones and adding/modifying methods and slots.

## Design Decisions

| Decision | Choice |
|---|---|
| Name | Clay (`.clay` files, `clay` compiler) |
| Paradigm | Pure OO, prototype-based, message-passing |
| Syntax | IO-style: `obj method(arg1, arg2)` |
| Memory | Reference counting (deterministic, readable C output) |
| Types | Fully dynamic (runtime dispatch) |
| Primitives | Tagged pointers for small ints/bools |
| Closures | First-class blocks: `{ \|x\| x + 1 }` |
| Dispatch | Hash map lookup on object + parent chain walk |
| Error handling | `doesNotUnderstand(name, args)` handler, default aborts |
| Compiler written in | C |
| C standard | C99 (widely supported, allows `//` comments and mixed decls) |
| Modules | Simple file-based `import("path")` for v1 |
| Concurrency | None for v1 (single-threaded) |
| Hash table | [mseminatore/hash](https://github.com/mseminatore/hash) — open addressing, string keys via `HT_HASH_STRING`, auto-grow |
| Test framework | [mseminatore/testy](https://github.com/mseminatore/testy) — single header+c file, MODULE/SUITE/TEST macros |

## Syntax Sketch

```clay
// Clone an object, add slots and methods
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
p toString() println()

// Blocks and control flow
numbers each({ |n| n println() })
(x > 0) ifTrue({ "positive" println() })

// Binary operators are messages
a + b        // sends '+' message to a with arg b
```

## Generated C (Target Readability)

```c
/* Point = Object clone({ x = 0, y = 0, ... }) */
ClayObject* Point = clay_clone(clay_Object);
clay_set_slot(Point, "x", clay_int(0));
clay_set_slot(Point, "y", clay_int(0));
clay_set_method(Point, "moveTo", clay_method_Point_moveTo);
clay_set_method(Point, "toString", clay_method_Point_toString);

/* p = Point clone() */
ClayObject* p = clay_send(Point, "clone", 0, NULL);

/* p moveTo(3, 4) */
clay_send(p, "moveTo", 2, (ClayValue[]){ clay_int(3), clay_int(4) });
```

## Architecture

```
.clay source
    |
    v
 [Lexer]  --- tokens --->  [Parser]  --- AST --->  [CodeGen]  --- .c file
                                                          |
                                                          v
                                                   [C Compiler]  --- binary
                                                          |
                                                          v
                                                  [Clay Runtime]  (linked)
```

The compiler is a pipeline: lex -> parse -> codegen -> invoke system C compiler. The runtime is a static library linked into every Clay program.

## Project Structure

```
clay/
  Makefile
  deps/
    hash/             # git submodule: mseminatore/hash
    testy/            # git submodule: mseminatore/testy
  src/
    runtime/
      clay.h          # Public runtime API
      object.c        # ClayObject struct, clone, slot access
      memory.c        # Reference counting (retain/release)
      dispatch.c      # Message send, method lookup
      tagged.c        # Tagged pointer encoding/decoding
      string.c        # String object implementation
      number.c        # Number object methods
      block.c         # Block/closure implementation
      array.c         # Array object implementation
      bootstrap.c     # Initialize core objects (Object, Nil, true, false)
    compiler/
      main.c          # CLI driver
      lexer.h / lexer.c
      parser.h / parser.c
      ast.h / ast.c
      codegen.h / codegen.c
  test/
    test_runtime.c      # C-level runtime tests (uses testy)
    programs/           # .clay test programs
      hello.clay
      point.clay
      blocks.clay
```

## Implementation Phases

### Phase 1: Runtime Foundation
1. **project-setup** — Directory structure, Makefile with targets for runtime lib, compiler, and tests. Clone mseminatore/hash and mseminatore/testy as git submodules under deps/
2. **object-model** — ClayObject struct using hash library (HashTable) for slots and methods, parent pointer, refcount. clay_clone(), clay_set_slot(), clay_get_slot()
4. **memory-refcount** — clay_retain(), clay_release() with free-on-zero, slot/method cleanup on dealloc
5. **tagged-values** — ClayValue union type, tagged small integers, Nil/true/false singletons
6. **message-dispatch** — clay_send() with parent chain walk and doesNotUnderstand fallback

### Phase 2: Core Objects
7. **bootstrap-objects** — Initialize Object, Nil, Boolean, Number, String, Array prototypes
8. **number-methods** — Arithmetic, comparison, toString()
9. **string-methods** — Concatenation, length(), println(), toString()
10. **array-methods** — append(), at(), size(), each(block)
11. **block-impl** — Closures as objects wrapping C function pointers + captured environment

### Phase 3: Compiler
12. **lexer** — Tokenize .clay files
13. **ast** — AST node type definitions
14. **parser** — Recursive descent parser producing AST
15. **codegen** — AST to C code with #line directives
16. **compiler-driver** — clay CLI orchestrating the full pipeline

### Phase 4: Integration & Testing
17. **hello-world** — First end-to-end test
18. **test-programs** — Point, fibonacci, blocks, prototype chains
19. **debug-support** — Verify #line directives, add clay_inspect() helper

## Sensible Defaults (Not Yet Decided in Detail)
- **Comments:** `//` line comments, `/* */` block comments
- **Statement separators:** Newlines; semicolons optional
- **Operator precedence:** Standard math precedence for binary operators; message sends left-to-right

## Phase 5: self and import

20. **self-keyword** — Verify `self` works inside methods and blocks. Add a test program using `self` explicitly (e.g., a method returning `self` for chaining). Ensure `self` outside a method produces a clear error.
21. **import-statement** — Implement `import("path")` as textual AST inclusion. Parser reads the imported file, parses it, and splices its statements before the import site. Guard against double-import. Add a test with a multi-file program.

## Phase 6: Language Enrichment

22. **string-interpolation** — Add `"x is ${expr}"` syntax. Lexer tokenizes interpolated strings into a series of string fragments and expressions. Parser builds a NODE_INTERP_STRING node containing alternating literal/expression children. Codegen emits a chain of string concatenations using `clay_string_concat` or a buffer-based approach. Add tests: simple var interpolation, expression interpolation, nested messages, empty interpolation.

23. **while-loops** — Add `{ condition } whileTrue({ body })` as a method on Block. Runtime: add `whileTrue` method to Block prototype that repeatedly evaluates the receiver block, checks for truthy result, and evaluates the body block. No compiler changes needed — this is pure runtime. Add test program exercising while loops (e.g., countdown, accumulator).

24. **logical-operators** — Add `&&`, `||`, `!` operators. `&&` and `||` should short-circuit. Lexer: add TOK_AND, TOK_OR, TOK_NOT tokens. Parser: add logical precedence level (lower than comparison). Codegen: emit short-circuit C code (`&&`/`||` with CLAY_IS_TRUTHY macro). Runtime: add `CLAY_IS_TRUTHY(val)` macro — false for CLAY_NIL, CLAY_FALSE, clay_int(0); true otherwise. Add `not` method on Boolean. Tests: truth tables, short-circuit verification.

25. **prototype-chain-tests** — Add comprehensive test programs exercising deep prototype chains: 3+ levels of inheritance, method override at each level, slot inheritance/shadowing, `doesNotUnderstand` at leaf vs. root, diamond-like re-cloning. Both runtime unit tests and .clay integration tests.

26. **string-methods** — Extend String prototype with: `at(index)` returning single-char string, `length()` (already exists — verify), `split(delimiter)` returning Array of strings, `contains(substr)` returning Boolean, `replace(old, new)` returning new String, `slice(start, end)` returning substring. Add runtime unit tests and a .clay test program.

## Phase 7: REPL (DONE)

27-30. **REPL** — Interactive read-eval-print loop. Accumulate & recompile approach. Variables persist, bare expressions auto-print, multi-line input, error recovery.

## Phase 8: Error Handling (Hybrid)

Combine overridable `doesNotUnderstand` with `try`/`catch` blocks for comprehensive error handling.

### Syntax

```clay
// try/catch blocks
result = try({
  10 / 0
}) catch({ |error|
  "Caught: ${error message()}" println()
  0
})

// Custom doesNotUnderstand
MyProxy = Object clone({
  doesNotUnderstand(name, args) {
    "forwarding ${name}" println()
  }
})

// Raise errors from user code
raise("something went wrong")
```

### Tasks

31. **error-object** — Create Error prototype with `message()` and `type()` methods. Add `setjmp`/`longjmp` error stack to runtime: a thread-local stack of `jmp_buf` frames. Implement `clay_raise(ClayValue error)` that longjmps to the nearest try frame, or aborts if none. Add `clay_Error` global prototype to bootstrap.

32. **convert-exits** — Replace all `exit(1)` calls in runtime with `clay_raise()`. Division by zero, modulo by zero, message-to-nil, value-on-non-block, method-not-found all become recoverable errors. Each creates an Error object with appropriate message and type string.

33. **try-catch-codegen** — Add `try` and `catch` keywords to lexer. Parser: `try(block) catch(block)` as an expression — try block is evaluated, if error is raised catch block receives the Error object as argument. Codegen: emit `setjmp`-based wrapper — push jmp_buf, call try block, pop on success; on longjmp, call catch block with error. `raise(expr)` is parsed as a statement/expression that calls `clay_raise()`.

34. **custom-dnu** — Remove forced `exit(1)` from `object_doesNotUnderstand`. The dispatch code already walks the parent chain for `doesNotUnderstand` — just make the default implementation call `clay_raise()` instead of `exit(1)`, so user-defined overrides work and uncaught errors still abort cleanly.

35. **error-tests** — Unit tests: try/catch with div-by-zero, method-not-found, custom raise, nested try/catch, uncaught error aborts. Clay test programs: error recovery, custom doesNotUnderstand proxy. Update README.

## Phase 9: VS Code Extension

Create a VS Code language extension for Clay at `editors/vscode/clay-lang/`.

### Tasks

36. **vscode-package** — Create `package.json` with extension metadata: `clay-lang` identifier, `.clay` file association, language configuration reference, grammar reference. Create `language-configuration.json` with bracket pairs, auto-closing pairs, comment rules (`//` line, `/* */` block), and surrounding pairs.

37. **vscode-grammar** — Create `syntaxes/clay.tmLanguage.json` TextMate grammar covering:
    - Keywords: `self`, `import`, `try`, `catch`, `raise`, `true`, `false`, `nil`
    - Comments: `//` line and `/* */` block
    - Strings: double-quoted with `${...}` interpolation highlighting
    - Numbers: integer literals
    - Operators: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`, `!`, `=`
    - Identifiers: capitalize-first as types/prototypes (e.g., `Object`, `Number`), lowercase as variables
    - Blocks: `{ |params| body }` with pipe-delimited param highlighting
    - Method definitions: `name(params) { body }` inside clone bodies
    - Builtins: `Object`, `Number`, `String`, `Array`, `Block`, `Boolean`, `Error`, `Nil`

38. **vscode-snippets** — Create `snippets/clay.json` with snippets for: clone object, method definition, block, try/catch, while loop, ifTrue/ifFalse, import.

39. **vscode-readme** — Create README in the extension folder with installation instructions (symlink or copy to `~/.vscode/extensions/`), feature list, and screenshots placeholder. Update project README with a mention.

## Phase 10: HashMap/Dict Object

Expose hash tables at the Clay level as a first-class `Dict` object.

### Syntax

```clay
// Dict literal: { key: value, ... }
d = Dict clone()
d set("name", "Clay")
d get("name") println()       // "Clay"
d has("name") println()       // true
d remove("name")
d size() println()            // 0

// Iteration
d each({ |key, value|
  "${key}: ${value}" println()
})

// toString
d toString() println()        // {name: Clay, version: 1}
```

### Tasks

40. **dict-runtime** — Create `src/runtime/dict.c` with Dict prototype:
    - `CLAY_TYPE_DICT` added to ClayType enum in clay.h
    - Union member: `struct { HashTable *map; } dict;` 
    - Constructor: `clay_dict_new()` — creates Dict with empty HashTable (string keys)
    - C helpers: `clay_dict_set(dict, key_str, value)`, `clay_dict_get(dict, key_str)`, `clay_dict_has(dict, key_str)`, `clay_dict_remove(dict, key_str)`, `clay_dict_size(dict)`
    - Methods: `set(key, val)`, `get(key)`, `has(key)`, `remove(key)`, `size()`, `keys()` (returns Array), `values()` (returns Array), `each(block)`, `toString()`
    - Reference counting: retain values stored, release on remove/dealloc
    - Init: `clay_dict_init_methods()` called from bootstrap, `clay_Dict` global prototype
    - Add to CMakeLists.txt, compiler driver file lists

41. **dict-codegen** — Add "Dict" to codegen builtin globals (`is_builtin_global`, `builtin_c_expr`). Add `clay_Dict` extern to clay.h. Register in bootstrap.

42. **dict-tests** — Unit tests in test_runtime.c: create, set/get, has/remove, size, keys/values, each, toString. Clay test program exercising Dict. Update README with Dict docs.

## Phase 11: Bytecode VM REPL (using vmgen) ✅

### Problem
The current REPL works by accumulating Clay source, recompiling to C, invoking MSVC, and re-executing the entire program on every input. This is slow (~2-3 seconds per keystroke round-trip) and fragile.

### Proposed Approach
Use **vmgen** (`C:\dev\vmgen`) to generate a bytecode VM from a declarative YAML spec, then add a bytecode compiler backend to Clay. The REPL would compile Clay expressions to bytecode and execute them instantly in-process — no C compilation, no subprocess.

### How vmgen fits

vmgen reads a YAML spec describing an instruction set, value types, and runtime features, then generates a complete standalone C VM implementation including: dispatch loop, assembler, disassembler, GC, closures, native function interface, debug stepping, and build system.

**What we'd get for free:**
- Stack-based dispatch loop (switch or computed gotos)
- Tagged union value representation (int64, double, bool, string, object)
- Mark-sweep garbage collection
- Closure/upvalue capture
- Text assembler (`.asm` → bytecode)
- Disassembler and debug tracing
- Bytecode binary save/load
- Native C function interface (for bridging Clay runtime)
- Generated CMakeLists.txt

**Integration architecture:**
```
Clay Source → [Lexer] → [Parser] → [AST] →┬→ [C CodeGen]   → .c file → MSVC → binary  (compile mode)
                                            └→ [BC CodeGen]  → bytecode → VM   → result  (REPL mode)
```

The existing lexer, parser, and AST are reused. Only a new bytecode codegen backend is needed, plus a Clay VM spec for vmgen.

### Implementation Status (DONE)

- ✅ **clay-vm-spec** — `src/vm/clay.yaml` with 22 opcodes, `clayvm` prefix to avoid runtime collision
- ✅ **generate-clay-vm** — 22 generated files in `src/vm/generated/`, compiled as `clayvm` library
- ✅ **bytecode-codegen** — `src/compiler/bc_compiler.c` compiles AST to bytecode. Handles: int/string literals, identifiers (global load), assignments (global store), binary ops, message sends, clone with slots
- ✅ **native-bridge** — 6 native functions: send, clone, set_slot, println, load_global, store_global. Clay runtime prototypes pre-registered as globals.
- ✅ **vm-repl** — `clay --vm-repl` launches bytecode REPL. Variables persist, auto-print results, error recovery via setjmp/longjmp.

### Key decisions made
- **Object model sharing**: VM wraps `ClayObject*` in `CLAYVM_VAL_OBJECT`. All Clay operations go through CALL_NATIVE to bridge functions.
- **Slot access fallback**: Added to `dispatch.c` — when method lookup fails, checks slots before `doesNotUnderstand`. This enables bytecode-created objects to work without explicit accessor methods.
- **Byte order**: Generated VM uses little-endian `READ_UINT16`, so `emit_u16` writes little-endian to match.
- **Generated VM fixes**: PUSH_CONST and DUP were coercing all values to INT64 — fixed to preserve value types.

### Known limitations
- Blocks/closures not yet compiled to bytecode (emit nil) — **Phase 12 addresses this**
- Array literals not compiled — **Phase 12 addresses this**
- Method definitions in clone bodies skipped — **Phase 12 addresses this**
- try/catch/raise not handled in bytecode
- No import handling
- No local variable scoping (everything is global)

## VS Code Integration ✅

Full IDE support via `.vscode/` configuration:

- **settings.json** — CMake auto-configure on open, `.clay` file association, hide build artifacts
- **tasks.json** — 6 tasks: CMake Configure, Build All (default build, Ctrl+Shift+B), Run Tests (default test), Compile Current .clay File, Compile to C Only, Launch VM REPL
- **launch.json** — 5 debug configs: Test Runner, VM REPL, Compile .clay File (debug the compiler), Run Compiled .clay Program (debug user code), Classic REPL
- **c_cpp_properties.json** — IntelliSense with include paths for runtime, compiler, VM, and deps

**Prerequisites:** CMake Tools + C/C++ extensions (both installed).

**Compiled-mode debugging:** The C codegen emits `#line` directives mapping generated C back to `.clay` source lines. The MSVC debugger shows stepping through the `.clay` file at statement level when using "Debug: Run Compiled .clay Program".

## Phase 12: VM Blocks, Closures & Method Definitions ✅

### Implementation (DONE)

All block/closure features implemented and tested in the VM REPL:

- **Block chunks**: Block bodies compile to separate heap-allocated bytecode chunks in `bc->chunks[]`
- **Block trampoline**: `bc_block_trampoline` re-enters VM to execute chunks (re-entrant safe — saves/restores full VM state)
- **Method trampolines**: 32 static C functions via `METHOD_TRAMP(N)` macro for `clay_set_method` registration
- **Closures**: Blocks access globals directly (reference semantics). Param names strdup'd to survive AST free.
- **Array literals**: `[1, 2, 3]` creates array + appends elements via send
- **Method defs in clone**: `Object clone({ getX() { self x() } })` compiles body to chunk, registers via method trampoline

### Verified working
- `{ |x| x * 2 } value(5)` → 10
- `(3 > 1) ifTrue({ "yes" }) ifFalse({ "no" })` → "yes"
- `[10, 20, 30] each({ |item| item println() })` → prints 10, 20, 30
- `Object clone({ x = 0; getX() { self x() } })` → method dispatch works
- `x = 10; { x + 1 } value()` → 11 (closure via globals)
- `{ i < 5 } whileTrue({ sum = sum + i; i = i + 1 })` → sum=10

### Known limitations
- No true closure capture (blocks read globals directly)
- Method trampoline pool limited to 32 methods
- Block code buffer limited to 4096 bytes
- Nested blocks may have param name collision via globals

## Phase 13: Clay Source-Level Debugging ✅ COMPLETE

All 3 debugging tasks implemented:
- `--trace` flag for bytecode tracing
- `--step` flag for interactive stepping with source line display
- Step commands: n (next), c (continue), s (stack), q (quit)
- Source line mapping via `line_map[]` in BcCompiler

Committed as `b273bf4`.

## Documentation Update (Needed)

The README.md is outdated and needs these updates:

1. **CLI options table** — Add `--vm-repl`, `--repl`, `--help`; document that no-args defaults to VM REPL
2. **REPL section** — Add VM REPL section showing `>>>` prompt, instant evaluation, blocks/closures working. Keep classic REPL as alternative (`--repl`)
3. **Architecture diagram** — Add bytecode VM path alongside compile-to-C path
4. **VS Code section** — Add build tasks (Ctrl+Shift+B), debug configurations (5 launch configs), not just syntax extension

## Phase 14: Floating-Point Numbers ✅

### Problem
Clay only supports tagged integers. There is no float/double type — `3.14` is a parse error, `10 / 3` returns `3` (integer division), and there's no way to represent fractional values. The VM's generated value type has `CLAYVM_VAL_DOUBLE` but the Clay runtime has no float path.

### Design
Floats will be heap-allocated `ClayObject` values with `CLAY_TYPE_FLOAT`, since `ClayValue` is a tagged `uintptr_t` that can't hold a float inline. Integer arithmetic stays fast (tagged, no allocation). Mixed int/float operations promote to float.

### Syntax
```clay
x = 3.14
y = 2.0
z = x + y          // 5.14
10 / 3              // 3 (integer division)
10 toFloat / 3      // 3.333...
3.14 toString       // "3.14"
3.14 toInt          // 3
```

### Tasks

1. **float-lexer** — Add `TOK_FLOAT` token. Lexer recognizes `digits.digits` (e.g., `3.14`, `0.5`, `.25`). Distinguish from `digits` (TOK_INT).

2. **float-ast** — Add `NODE_FLOAT_LIT` AST node with `float` value. Parser creates it for `TOK_FLOAT` tokens.

3. **float-runtime** — Add `CLAY_TYPE_FLOAT` to ClayType enum. Add `struct { float value; } number_float;` to ClayObject union. Add `clay_float_new(float)`, `clay_to_float(ClayValue)`, `CLAY_IS_FLOAT(v)` helpers. Float objects have parent=clay_Number.

4. **float-number-methods** — Update Number prototype arithmetic (`+`, `-`, `*`, `/`, `%`, comparisons) to handle mixed int/float: if either operand is float, promote both to float and return float. Integer-only operations stay tagged. Add `toFloat` method on integers, `toInt` method on floats (truncation).

5. **float-codegen** — C codegen emits `clay_float_new(3.14f)` for float literals. Generated C is still readable.

6. **float-bc-compiler** — Bytecode compiler handles `NODE_FLOAT_LIT`: push as `CLAYVM_VAL_DOUBLE` constant (VM uses double internally, Clay wraps as float).

7. **float-tostring** — Float `toString` formats with minimal precision (no trailing zeros). E.g., `3.14` not `3.140000`.

8. **float-tests** — Unit tests: float creation, arithmetic, mixed int/float, comparisons, toString, toInt/toFloat conversions. Clay test program with float math. VM REPL verification.

### Dependencies
```
float-lexer → float-ast → float-runtime → float-number-methods → float-codegen
                                         → float-bc-compiler
                                         → float-tostring
                                                              → float-tests
```

## Phase 15: Standard Library Enhancements

### Problem
Clay's standard library is minimal — Arrays lack `map`/`filter`/`sort`, Strings can't parse numbers or change case, there's no math, no console input, no file I/O, and no type introspection. This limits Clay's usefulness as a general-purpose language.

### 15a: Type Checking

Add a `type` method to Object that returns the type name as a String.

```clay
42 type            // "Number"
3.14 type          // "Float"
"hello" type       // "String"
[1, 2] type        // "Array"
true type          // "Boolean"
nil type           // "Nil"
{ 1 } type         // "Block"
Object clone() type // "Object"
```

**Tasks:**
1. **type-method** — Add `type` method to Object prototype returning type name string. Handle tagged ints (via dispatch wrapper), floats, strings, arrays, blocks, booleans, nil, dicts, errors, and plain objects. Works in compiled mode and VM REPL.

### 15b: Math Library

Add a `Math` object with common math functions. Uses C `<math.h>` internally.

```clay
Math abs(-5)          // 5
Math sqrt(16.0)       // 4
Math pow(2, 10)       // 1024
Math min(3, 7)        // 3
Math max(3, 7)        // 7
Math floor(3.7)       // 3
Math ceil(3.2)        // 4
Math round(3.5)       // 4
Math random           // 0.0–1.0 (float)
Math pi               // 3.14159...
```

**Tasks:**
2. **math-object** — Create `src/runtime/math.c` with `clay_Math` global object. Methods: `abs`, `sqrt`, `pow`, `min`, `max`, `floor`, `ceil`, `round`, `random`, `sin`, `cos`. Slots: `pi`, `e`. Register in bootstrap, add to codegen builtins, add to VM REPL globals.

### 15c: Array Methods

Extend Array with functional-style operations.

```clay
arr = [1, 2, 3, 4, 5]
arr map({ |x| x * 2 })           // [2, 4, 6, 8, 10]
arr filter({ |x| x > 2 })        // [3, 4, 5]
arr reduce(0, { |acc, x| acc + x }) // 15
arr find({ |x| x > 3 })          // 4
arr sort                          // [1, 2, 3, 4, 5]
arr reverse                      // [5, 4, 3, 2, 1]
arr first                        // 1
arr last                         // 5
arr join(", ")                   // "1, 2, 3, 4, 5"
arr contains(3)                  // true
arr indexOf(3)                   // 2
```

**Tasks:**
3. **array-functional** — Add `map`, `filter`, `reduce`, `find` to Array. Each takes a block argument, returns a new array (or value for reduce/find).
4. **array-utility** — Add `sort` (natural ordering), `reverse`, `first`, `last`, `join(separator)`, `contains(val)`, `indexOf(val)`, `removeAt(i)`, `length` (alias for size).

### 15d: String Methods

Extend String with parsing and case conversion.

```clay
"42" toInt               // 42
"3.14" toFloat           // 3.14
"  hello  " trim         // "hello"
"hello" upper            // "HELLO"
"HELLO" lower            // "hello"
"hello" startsWith("he") // true
"hello" endsWith("lo")   // true
"hello" reverse          // "olleh"
"hello" charAt(0)        // 104 (ASCII code)
```

**Tasks:**
5. **string-parsing** — Add `toInt` and `toFloat` on String. Return the parsed number or raise an error on invalid input.
6. **string-case** — Add `trim`, `upper`, `lower`, `startsWith`, `endsWith`, `reverse`.

### 15e: Console I/O

Add console input methods for interactive programs.

```clay
name = readLine               // reads a line from stdin
age = prompt("Your age? ")    // prints prompt, reads line
```

**Tasks:**
7. **console-input** — Add `readLine` as a global function (or method on a Console/System object). Add `prompt(msg)` that prints the message and reads a line. Returns a String. Works in compiled mode and VM REPL.

### 15f: File I/O

Add file reading/writing for basic scripting capability.

```clay
// Reading
contents = File read("data.txt")
lines = File readLines("data.txt")

// Writing
File write("output.txt", "hello world")
File append("log.txt", "new entry\n")

// Checking
File exists("data.txt")        // true/false
```

**Tasks:**
8. **file-io** — Create `src/runtime/file.c` with `clay_File` global object. Methods: `read(path)` (returns String), `readLines(path)` (returns Array of Strings), `write(path, content)`, `append(path, content)`, `exists(path)` (returns Boolean). Register in bootstrap, codegen builtins, VM REPL globals.

### Implementation Order
```
type-method → math-object → string-parsing → string-case
                           → array-functional → array-utility
                           → console-input → file-io
```

Start with type-method (smallest, most useful for debugging), then math and array/string extensions (pure library work), then I/O (requires more design decisions).

## Phase 15 Status: ✅ COMPLETE
All 8 tasks implemented and committed.

## Phase 16: String Escape Sequences

**Problem:** Clay string literals do not process C-style escape sequences. `"hello\nworld"` stores the literal characters `\n` (backslash + n), not a newline. This affects Console I/O, File I/O, and general string use.

**Approach:** Add escape processing in the parser's `token_strdup` path for string literals (or a dedicated `process_escapes` helper). The lexer already skips `\X` pairs correctly — we just need to translate them when building the string value.

**Supported escapes:**
- `\n` → newline (0x0A)
- `\t` → tab (0x09)
- `\r` → carriage return (0x0D)
- `\\` → literal backslash
- `\"` → literal double quote
- `\0` → null byte

## Phase 16 Status: ✅ COMPLETE
String escape sequences implemented and committed.

## Phase 16+ (Future)
- Optional type annotations
- Inline caching for dispatch performance
- Self-hosting compiler
- Concurrency (actors or green threads)
- Package manager
- DAP debug adapter for VS Code native breakpoints/watches

## Phase 17: Slot Assignment & Constructors ✅

### Problem
Clay objects have slots (data fields), but there's no way to read or write them from Clay code after creation. Slots can only be set inside `clone({...})` bodies. This means:
- No mutable object state (`p x = 5` doesn't work)
- No constructor pattern (`Point new(3, 4)` can't set slots)
- Objects are effectively immutable after creation

### Research: How Other Languages Handle This

**Io** (closest to Clay's design):
- `:=` creates a new slot: `obj name := "Alice"`
- `=` updates an existing slot: `obj name = "Bob"` (error if slot doesn't exist)
- `init` method is auto-called after `clone`: `Dog := Animal clone` triggers `Dog init`
- Slot access is a message: `obj name` returns the slot value (no parens needed)

**Self** (pioneer of prototype OOP):
- Slots have paired getter/setter: defining slot `x` creates `x` (getter) and `x:` (setter)
- `obj x: 5` calls the setter, `obj x` calls the getter
- `copy` + slot assignment replaces constructors

**Smalltalk** (class-based, but Clay's syntax ancestor):
- `obj x: 5` keyword message for setting
- `initialize` method called by `new`
- Factory methods: `Point x: 3 y: 4`

**JavaScript** (prototype-based, familiar):
- `obj.x = 5` direct property assignment
- `constructor()` in classes, or manual init in prototype pattern

### Design for Clay

**Approach: Io-inspired slot access + Self-inspired setter messages**

Clay already resolves unknown messages via `doesNotUnderstand`. We can make slot access automatic:

1. **Slot read as message** — `obj x` already works if `x` is a method. Make it also check slots. If a slot `x` exists, return its value. This is the "slot-as-getter" pattern.

2. **Slot write via setter message** — `obj setX(5)` calls a generated or runtime setter. Alternatively, support `obj x = 5` syntax (parsed as assignment-to-slot-on-receiver).

3. **Constructor convention** — `init` method auto-called after clone (Io pattern). Or a `new(args)` factory method pattern.

**Syntax options for slot assignment:**

```clay
// Option A: setter messages (Io/Self style)
p setX(5)                    // explicit setter
p x = 5                      // sugar for setX(5) — needs parser work

// Option B: direct slot syntax (Io := style)
p x := 5                     // creates/updates slot

// Option C: slot send with assignment (simplest parser change)
p x = 5                      // receiver slot assignment
```

**Recommended: Option C** (`obj slot = value`)
- Minimal new syntax — `receiver ident = expr` when receiver is explicit
- Consistent with existing `x = 5` for locals/globals
- `p x` reads the slot (falls through dispatch to slot lookup)
- `p x = 5` writes the slot

**Constructor pattern:**

```clay
Point = Object clone({
  x = 0
  y = 0
  init(x, y) {
    self x = x
    self y = y
  }
})

p = Point new(3, 4)     // clone + call init
```

Where `new(args...)` is a built-in method on Object that does:
1. `clone` self
2. Call `init(args...)` on the clone (if init exists)
3. Return the clone

### Tasks

1. ~~**slot-read** — Dispatch falls through to `clay_get_slot()` when no method found (dispatch.c).~~ ✅ DONE
2. ~~**slot-write-syntax** — `NODE_SLOT_ASSIGN` in parser for `receiver ident = expr`.~~ ✅ DONE
3. ~~**slot-write-codegen** — `clay_set_slot` emission in codegen.c and bc_compiler.c.~~ ✅ DONE
4. ~~**object-new** — `new(args...)` on Object: clone + call init. Fixed VM re-entrancy (stack save/restore in bc_method_dispatch).~~ ✅ DONE (commit 4bb4ba2)
5. **constructor-tests** — Test full pattern: prototype with init, new(), slot read/write, inheritance of init, new with no init.
6. **slot-docs** — Update README with slot access, slot assignment, constructor patterns, and examples.

### Examples (target behavior)

```clay
// Define a prototype with slots and constructor
Point = Object clone({
  x = 0
  y = 0
  init(x, y) {
    self x = x
    self y = y
  }
  toString() {
    "(" + x toString + ", " + y toString + ")"
  }
})

p = Point new(3, 4)
p x                          // 3
p y                          // 4
p toString                   // "(3, 4)"
p x = 10                     // mutate slot
p x                          // 10

// Inheritance works
Point3D = Point clone({
  z = 0
  init(x, y, z) {
    self x = x
    self y = y
    self z = z
  }
})

q = Point3D new(1, 2, 3)
q z                          // 3
```

## Phase 18: Refcount Correctness & Memory Diagnostics ✅

### Problem
Multiple refcounting bugs exist across the VM REPL and runtime:
1. **Global variable assignment** doesn't retain/release — `Cat = Dog` leaves refcount at 1
2. **toString leaks** — temporary strings from `toString` calls never released in several places
3. **Singleton list incomplete** — `clay_is_singleton()` missing Error, Math, Console, File prototypes
4. **No memory diagnostics** — no way to validate refcounts or detect leaks

### Approach
Fix all refcount bugs, then add `--mem-stats` flag for leak detection.

### Tasks

1. **fix-store-global** — In `native_store_global_fn` (bc_compiler.c): retain new object values, release old object values when overwriting. Add helper to check if a clayvm_value_t holds an object pointer.

2. **fix-tostring-leaks** — Release temporary toString results in:
   - `native_println_fn` (bc_compiler.c)
   - REPL auto-print in main.c (two places: step mode + normal mode)
   - `array_toString` (array.c)
   - `array_join` (array.c)
   - `array_sort_compare` (array.c)
   - `dict_toString` (clay_dict.c)

3. **fix-singleton-list** — Add `clay_Error`, `clay_Math`, `clay_Console`, `clay_File` to `clay_is_singleton()` in memory.c.

4. **add-mem-stats** — Add global allocation/deallocation counters in memory.c. Add `--mem-stats` CLI flag that prints object stats on REPL exit.

## Phase 19: Cross-Platform CI

### Problem
Clay is developed on Windows but the codebase targets cross-platform (C99, CMake, `#ifdef _WIN32` paths). There is no automated CI to catch platform-specific build failures or test regressions. Breakage on Linux/macOS could go unnoticed.

### Approach
Add a GitHub Actions workflow that builds and tests on Windows, Linux, and macOS. Use the CMake build system already in place. The workflow should:
- Check out with submodules (deps/hash, deps/testy are git submodules)
- Configure, build, and run `ctest` on all three platforms
- Run on push to main and on pull requests

### Tasks

1. **ci-workflow** — Create `.github/workflows/ci.yml` with a matrix strategy:
   - `os: [ubuntu-latest, macos-latest, windows-latest]`
   - Steps: checkout (with submodules), cmake configure, cmake build, ctest
   - Use `Debug` config on Windows (MSVC multi-config), default on Unix
   - Trigger on: push to main, pull_request

2. **fix-platform-issues** — Fix any build/test failures revealed by CI on Linux/macOS:
   - Check Unix compile path in `main.c` (`cc` command) — may need `-lm` for math
   - Check `strdup` availability (POSIX, not C99 — may need `_POSIX_C_SOURCE`)
   - Check `_popen`/`_pclose` vs `popen`/`pclose` usage
   - Verify `#ifdef _WIN32` guards are complete

### Dependencies
```
ci-workflow → fix-platform-issues (CI reveals what needs fixing)
```

## Phase 20: Bricks — Clay Package Manager

### Problem
Clay has no way to share or reuse code beyond `import("path")` with relative file paths. There's no standard library distribution, no dependency resolution, and no way to discover/install community packages.

### Design

**Bricks** is Clay's package manager, built directly into the `clay` binary as subcommands. Each package (a "brick") is any Git repository containing Clay source files and a `brick.json` manifest. There is no central registry — any GitHub (or other Git) URL is a valid package source, similar to Go modules.

**Why arbitrary repos (no registry):**
- Zero infrastructure to maintain — no server, no account signup
- Any Git repo with a `brick.json` is instantly a valid brick
- GitHub topics/search provide natural discovery (`topic:clay-brick`)
- Proven model — Go modules and Deno work this way at massive scale

#### CLI (subcommands of `clay`)
```
clay bricks init                              # create brick.json
clay bricks add github.com/user/some-lib      # add + install dependency
clay bricks install                           # install all deps from brick.json
clay bricks list                              # show installed bricks
clay bricks remove some-lib                   # remove a dependency
```

No separate binary needed — everything is handled by the `clay` executable.

#### Package structure
```
github.com/user/my-brick/
  brick.json        ← manifest
  src/              ← Clay source files
    main.clay
    utils.clay
  test/             ← optional tests
    test_utils.clay
  README.md
```

#### Manifest (`brick.json`)
```json
{
  "name": "my-brick",
  "version": "0.1.0",
  "description": "A useful Clay library",
  "main": "src/main.clay",
  "deps": {
    "some-lib": "github.com/user/some-lib@v0.2.0"
  }
}
```

- `name`: package name (kebab-case)
- `version`: semver string
- `main`: entry point file (what `import("my-brick")` loads)
- `deps`: dependency map — `name → git_url@tag_or_branch`

URLs can be any Git-cloneable URL: `github.com/user/repo`, `gitlab.com/user/repo`, or even local paths. The `@ref` suffix specifies a tag, branch, or commit to check out.

#### How it works

1. **`clay bricks init`** — Create a `brick.json` in the current directory
2. **`clay bricks add <url>`** — Clone the repo into `bricks/`, read its `brick.json`, add to project deps
3. **`clay bricks install`** — Clone/update all dependencies from `brick.json` into `bricks/`
4. **`import("brick-name")`** — Resolve via `bricks/<name>/` directory, load the brick's `main` entry point

#### Local layout after install
```
my-project/
  brick.json
  main.clay
  bricks/               ← gitignored, created by `clay bricks install`
    some-lib/            ← cloned from github
      brick.json
      src/main.clay
    another-lib/
      ...
```

#### Import resolution (updated)
Current: `import("path")` resolves relative to the importing file.
New: if the path has no `/` or `\` and no `.clay` extension, check `bricks/<name>/` and load its `main` from `brick.json`.

```clay
import("utils.clay")       // relative file import (unchanged)
import("some-lib")         // brick import → bricks/some-lib/src/main.clay
```

### Tasks

1. **brick-manifest** — Define `brick.json` schema. Add parsing in a new `src/compiler/bricks.c` file. Parse `name`, `version`, `main`, `deps` fields. Use a simple JSON subset parser (or just key-value scanning since the format is controlled).

2. **brick-init** — `clay bricks init` command: creates a `brick.json` template in the current directory with name derived from the folder name.

3. **brick-install** — `clay bricks install` command: read `brick.json`, for each dep run `git clone <url> bricks/<name>` (or `git pull` if already exists). Handle `@tag` by checking out the specified tag/branch after clone.

4. **brick-add** — `clay bricks add github.com/user/lib` command: clone the repo to `bricks/`, read its `brick.json` for the name, add to the current project's `brick.json` deps.

5. **brick-import** — Modify the import resolver (parser.c) to check `bricks/<name>/` when the import path has no extension. Read the brick's `brick.json` to find its `main` entry point. Fall back to `bricks/<name>/src/main.clay` if no manifest.

6. **brick-recursive** — Recursive dependency install: when installing a brick, also install its deps (from its `brick.json`). Detect and warn on circular dependencies.

7. **brick-docs** — Document Bricks in README: usage, brick.json format, creating/publishing bricks, import syntax.

### Dependencies
```
brick-manifest → brick-init
brick-manifest → brick-install → brick-add
brick-manifest → brick-import
brick-install → brick-recursive
All → brick-docs
```

### Design notes
- **Any Git URL** — not limited to GitHub. Works with GitLab, Bitbucket, self-hosted Git, or even local paths.
- **No complex version resolution** — each dep specifies an exact tag/branch. No semver ranges, no diamond dependency resolution. If two bricks need different versions of the same dep, last-installed wins (with a warning).
- **Git-based** — requires `git` on PATH. No custom download protocol.
- **`bricks/` directory** — analogous to `node_modules/`. Should be gitignored. Recreated from `brick.json` via `clay bricks install`.
- **Transitive deps** — `brick-recursive` handles installing deps of deps, but flattened into a single `bricks/` directory (not nested).
- **Single binary** — all `bricks` subcommands are built into the `clay` executable. No separate tool to install.
- **Future enhancements** — lockfile (`brick-lock.json`), semver range resolution, `clay bricks publish` (create GitHub release), `clay bricks search` (GitHub API query).
