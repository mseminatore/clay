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

## Phase 11: Bytecode VM REPL (using vmgen)

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

### Tasks

43. **clay-vm-spec** — Write `specs/clay.yaml` for vmgen defining Clay's bytecode instruction set:
    - Types: int64, double, bool, string, object, nil
    - Stack: dynamic, tagged_union
    - Features: mark_sweep GC, closures, interned_strings, debug_tracing
    - Instructions: PUSH_CONST, PUSH_NIL, PUSH_TRUE, PUSH_FALSE, POP, DUP, LOAD_LOCAL, STORE_LOCAL, LOAD_GLOBAL, STORE_GLOBAL, ADD, SUB, MUL, DIV, MOD, EQ, NEQ, LT, GT, LTE, GTE, NOT, NEGATE, JUMP, JUMP_IF_FALSE, CALL, RETURN, SEND (message dispatch), CLONE, SET_SLOT, GET_SLOT, SET_METHOD, MAKE_BLOCK, MAKE_ARRAY, RAISE, TRY_PUSH, TRY_POP, HALT
    - Native functions: println, print, toString (bridge to Clay runtime)

44. **generate-clay-vm** — Run vmgen on the spec to produce generated C files. Integrate into CMakeLists.txt as a library. Verify the generated VM compiles and basic smoke tests pass.

45. **bytecode-codegen** — New compiler backend (`src/compiler/bc_codegen.c`) that walks the AST and emits bytecode instead of C. Handles: literals, variables (local/global), message sends via SEND opcode, binary ops, blocks/closures, clone expressions, try/catch, raise. Manages a constant pool and local variable table.

46. **native-bridge** — Register Clay runtime functions (clay_clone, clay_send, clay_string_new, etc.) as native functions in the generated VM. The SEND opcode calls into the existing Clay dispatch system. This preserves full compatibility with the runtime object model.

47. **vm-repl** — Replace the accumulate-and-recompile REPL loop with: parse input → compile to bytecode → execute in persistent VM instance. Variables live in the VM's global table across inputs. Results auto-print. Errors caught by the VM's error handler (no process crash).

48. **vm-repl-tests** — Test the bytecode REPL: arithmetic, string operations, object creation, method calls, closures, error handling, multi-line input. Verify parity with the C-compilation REPL.

### Key design decisions needed
- **Object model sharing**: The VM's `object` type should wrap `ClayObject*` pointers, reusing the existing runtime. The SEND opcode delegates to `clay_send_val()`. This avoids reimplementing the entire object model.
- **GC coordination**: vmgen's mark-sweep GC manages VM stack roots; Clay's refcounting manages object lifetimes. Need a bridge where VM-reachable objects are retained.
- **Compilation mode**: `clay file.clay` still compiles to C (production). `clay` (no args) or `clay --repl` uses the bytecode VM. Both share the same parser.

## Phase 12+ (Future)
- HashMap/Dict object (expose hash tables at Clay level)
- Optional type annotations
- Inline caching for dispatch performance
- Self-hosting compiler
- Concurrency (actors or green threads)
- Package manager
