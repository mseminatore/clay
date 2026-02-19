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

## Phase 12: VM Blocks, Closures & Method Definitions

### Problem
The VM REPL is non-functional for real Clay code. Clay has **no control flow keywords** — `ifTrue`, `ifFalse`, `each`, `whileTrue` are all message sends with block arguments. Without block compilation, the VM REPL can't do conditionals, loops, iteration, or method definitions.

### Architecture
In compiled mode, each block becomes a static C function. For the bytecode VM:
- Block bodies compile to **separate bytecode chunks** stored in a side table
- A single static C **trampoline function** re-enters the VM to execute a chunk
- Captures are detected at compile time via `scan_free_vars()` and stored in the block object's `env` array
- `env[0]` holds the chunk ID (as tagged int), `env[1..n]` hold captured values
- A global `g_vm` pointer lets the trampoline access the VM instance

### Tasks

49. **vm-block-chunks** — Add bytecode chunk storage to BcCompiler. A chunk is: `{ uint8_t *code, size_t len, int param_count }`. Store in a growable array `bc->chunks[]` with a `chunk_count`. When compiling NODE_BLOCK, save current code buffer, compile body to a fresh buffer, store as a new chunk, restore the outer buffer.

50. **vm-capture-scan** — Port `scan_free_vars()` from `codegen.c` to `bc_compiler.c`. Given a block body AST and the current global variable names, identify free variables (identifiers used in body that aren't params, builtins, or block-local). Return a list of capture names.

51. **vm-block-trampoline** — Create `bc_block_trampoline(ClayObject *self, int argc, ClayValue *argv)` in `bc_compiler.c`:
    - Read chunk_id from `self->as.block.env[0]` (tagged int)
    - Look up bytecode chunk from global chunk table
    - Save VM state (sp, ip)
    - Push captured vars from `self->as.block.env[1..n]` as pseudo-locals (store to globals with mangled names)
    - Push params from `argv` as locals
    - Run the bytecode chunk
    - Restore VM state, return result

52. **vm-make-block** — In `bc_emit_expr` for NODE_BLOCK: compile body to a new chunk, scan for captures, emit bytecode that pushes chunk_id + captured values, calls `native_make_block(chunk_id, capture_count, captures...)`. The native creates a ClayObject block with the trampoline function + env array.

53. **vm-method-defs** — Compile NODE_METHOD_DEF inside NODE_CLONE. Methods are named blocks bound to an object. After creating the block object (same mechanism as task 52), call `clay_set_method(obj, name, block_trampoline)` to register it. Handle `self` access inside methods.

54. **vm-array-literal** — Compile NODE_ARRAY_LIT: emit `native_make_array()` to create an empty array, then for each element compile the value and emit `native_send(arr, "append", element)`.

55. **vm-block-tests** — Test full block functionality in VM REPL: `ifTrue`/`ifFalse`, `each` on arrays, closures with capture (`x = 10; f = { x + 1 }; f value()`), method definitions in clone, nested blocks. Verify the `blocks.clay` test program's expressions work in the REPL.

### Dependencies
```
vm-block-chunks → vm-capture-scan → vm-block-trampoline → vm-make-block → vm-method-defs
                                                                        → vm-array-literal
                                                        → vm-block-tests
```

## Phase 13: Clay Source-Level Debugging

### Problem
The VM REPL has no source-level debugging. The generated VM has `step_callback`, `trace_enabled`, and `clayvm_vm_step()` APIs but no line mapping from bytecode to `.clay` source.

### Tasks

49. **bytecode-tracing** — Add a `--trace` flag to the VM REPL that sets `vm.trace_enabled = 1`, printing each opcode as it executes (PUSH_CONST, CALL_NATIVE, etc.). Quick win for understanding execution flow. Add to `--help` output.

50. **source-line-mapping** — Record which `.clay` source line each bytecode instruction came from in `bc_compiler.c`. Use a parallel `int line_map[BC_MAX_CODE]` array that stores the AST node's `line` field for each emitted byte. When the VM's `step_callback` fires, look up the source line and print it (or the source text) when the line changes from the previous step. This gives line-by-line stepping in the VM REPL via a `--step` flag.

51. **stepping-commands** — Add interactive stepping commands to the VM REPL when in step mode: `n` (next line), `c` (continue to end), `s` (show current stack), `v` (show variables/globals), `q` (quit). Use `clayvm_vm_step()` for single-instruction execution, coalescing steps within the same source line for `n`.

### Design notes
- The generated VM already has `vm.trace_enabled`, `clayvm_vm_step()`, and `void (*step_callback)(void *vm, size_t ip)` — all generated by vmgen.
- Line mapping only needs to track the bytecode offset → source line relationship. The `linemap.c`/`linemap.h` generated files may already provide infrastructure for this.
- For DAP (Debug Adapter Protocol) integration in the future, this source mapping work is a prerequisite.

## Phase 14+ (Future)
- Optional type annotations
- Inline caching for dispatch performance
- Self-hosting compiler
- Concurrency (actors or green threads)
- Package manager
- DAP debug adapter for VS Code native breakpoints/watches
