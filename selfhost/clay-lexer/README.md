# clay-lexer

The Clay tokenizer, written in Clay — the first piece of a self-hosted Clay
compiler. It is a direct port of `src/compiler/lexer.c` and produces exactly
the same tokens: CTest (`clay_selfhost_lexer`, and `clay_selfhost_lexer_native`
for the natively compiled lexer) compares the two on every `.clay` file in the
repository plus the edge cases in `test/cases/`.

```clay norun
import("clay-lexer")                       // or a relative path to src/main.clay

lx = Lexer new("total = price * 2  // tax")
tok = lx next
"${tok kind} ${tok text} (line ${tok line})" println    // IDENT total (line 1)

(Lexer tokenize("a += 1") map { |t| t kind }) println  // [IDENT, +=, INT, EOF]
```

## Tokens

A `Token` has `kind`, `text` and `line`; `tok is("IDENT")` tests the kind.

- **kind** — the C lexer's type names: `INT`, `FLOAT`, `STRING`,
  `INTERP_STRING` (contains `${`), `IDENT`; keywords in upper case (`SELF`,
  `SUPER`, `IMPORT`, `TRY`, `CATCH`, `FINALLY`, `RAISE`, `RETURN`, `BREAK`,
  `CONTINUE`, `AS`); punctuation and operators as their own text (`(`, `#{`,
  `..`, `==`, `+=`, `??`, …); `NEWLINE`, `EOF` and `ERROR`.
- **text** — the token's source text. Strings hold the raw text between the
  quotes (escapes are not processed); `ERROR` holds the message.
- **line** — the line the lexer is on after reading the token, as in the C
  lexer: a `NEWLINE` token reports the next line, a multi-line string its last.

`Lexer tokenize(source)` returns every token up to and including `EOF`, or
stopping at the first `ERROR`.

## Comparing with the C lexer

```sh
clay --tokens file.clay                                  # C lexer
clay selfhost/clay-lexer/tools/tokens.clay file.clay     # this lexer
```

Both print one token per line as `<line> <kind> <text>`, with `\`, newline,
carriage return and tab escaped in the text.
