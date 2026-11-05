# clay-parser

The Clay parser, written in Clay — the second piece of a self-hosted Clay
compiler, on top of [clay-lexer](../clay-lexer). It is a port of
`src/compiler/parser.c`: the same grammar, the same desugarings, import
resolution and error messages. CTest (`clay_selfhost_parser`, and
`clay_selfhost_parser_native` for the natively compiled parser) compares its
output with `clay --ast` on every `.clay` file in the repository plus the
edge cases in `test/cases/` (including every syntax error message).

```clay norun
import("../clay-parser/src/main.clay")

p = Parser new("total = price * (1 + rate)", "example.clay")
program = p parseProgram
p hadError println                      // false
(AstPrinter print(program)) print
// Program(stmts=1) @1
//   Assign(total) @1
//     Binary(*) @1
//       Ident(price) @1
//       Binary(+) @1
//         Int(1) @1
//         Ident(rate) @1
```

## Nodes

Every node is a `Node` with `kind` and `line`; the other slots depend on the
kind:

| kind | slots |
|---|---|
| `Int`, `Float`, `String` | `value` |
| `Ident` | `name` |
| `Self` | — |
| `Assign` | `name`, `value` |
| `Send` | `recv`, `name` (selector), `items` (arguments), `isSuper` |
| `Binary` | `name` (operator), `left`, `right` |
| `Block` | `params`, `body` |
| `MethodDef` | `name`, `params`, `body` (a parameter with a default is an `Assign`) |
| `SlotDef` | `name`, `value` |
| `Clone` | `recv` (prototype), `items` (`SlotDef` / `MethodDef` members) |
| `Array` | `items` |
| `DictLit` | `keys` (`String` nodes), `vals` |
| `TryCatch` | `tryBlock`, `catchBlock`, `finallyBlock` (blocks; the last two may be nil) |
| `Raise` | `expr` |
| `SlotAssign` | `recv`, `name` (slot), `value` |
| `Return` | `expr` (nil for a bare `return`) |
| `Break`, `Continue` | — |
| `Program` | `items` (statements) |

The parser desugars as the C parser does: `x += e` becomes `x = x + e` (also
for slots), `[a, b] = e` becomes assignments through `__destr_src`, `-x`
becomes `0 - x` (literals are folded), `!x` becomes `x not`, and
`"a ${e} b"` becomes `"a " + e toString + " b"`. Imports are resolved and
spliced in; `import("x") as Ns` also builds the `Ns` object.

## Errors

The first syntax error is printed as `file:line: error: message (got 'token')`
and `hadError` becomes true; parsing stops there.

## Comparing with the C parser

```sh
clay --ast file.clay                                    # C parser
clay selfhost/clay-parser/tools/ast.clay file.clay      # this parser
```

Kept for compatibility with the C parser: a string literal ends at a `\0` or
`\x00` escape, `${...}` is found by counting braces in the raw text, and only
the `"main"` field of a `brick.json` is read.
