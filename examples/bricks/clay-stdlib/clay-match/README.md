# clay-match

Pattern matching for Clay with a chainable API.

## Usage

```clay
import("clay-stdlib/clay-match")
```

## Methods

```clay
// Value matching
result = Match value(x)
    when(1, { "one" })
    when(2, { "two" })
    else({ "other" })
    result()

// Type matching
result = Match value(x)
    whenType("String", { "it's a string" })
    whenType("Number", { "it's a number" })
    else({ "unknown" })
    result()

// Predicate matching
result = Match value(x)
    whenPred({ |v| v > 100 }, { "big" })
    whenPred({ |v| v > 10 }, { "medium" })
    else({ "small" })
    result()
```

| Method | Description |
|--------|-------------|
| `value(v)` | Start a match expression |
| `when(pattern, block)` | Match exact value |
| `whenType(typeName, block)` | Match by type name |
| `whenPred(pred, block)` | Match by predicate |
| `else(block)` | Default case (no match) |
| `result()` | Get the match result |

The first matching branch wins. If no branch matches and no `else` is provided, the result is `nil`.
