# clay-csv

CSV parsing and generation for Clay.

## Usage

```clay
import("clay-stdlib/clay-csv")
```

## Parsing

```clay
// Parse to array of arrays
rows = Csv parse("name,age\nAlice,30\nBob,25")
rows at(0)           // ["name", "age"]
rows at(1)           // ["Alice", "30"]

// Parse with headers (returns array of Dicts)
records = Csv parseWithHeader("name,age\nAlice,30\nBob,25")
records at(0) get("name")   // "Alice"
records at(0) get("age")    // "30"
records at(1) get("name")   // "Bob"
```

## Generation

```clay
// Generate CSV from array of arrays
csv = Csv stringify([["name", "age"], ["Alice", "30"]])
// "name,age\nAlice,30"
```

## Methods

| Method | Description |
|--------|-------------|
| `parse(str)` | Parse CSV to array of arrays |
| `parseWithHeader(str)` | Parse CSV to array of Dicts (first row = keys) |
| `stringify(rows)` | Convert array of arrays to CSV string |

Fields containing commas, quotes, or newlines are handled according to RFC 4180.
