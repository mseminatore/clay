# clay-json

JSON parsing and serialization for Clay.

## Usage

```clay
import("clay-stdlib/clay-json")
```

## Parsing

```clay
// Parse JSON string into Clay objects
obj = Json parse("{\"name\": \"Clay\", \"version\": 1}")
obj get("name")         // "Clay"
obj get("version")      // 1

// Arrays
arr = Json parse("[1, 2, 3]")
arr at(0)               // 1

// Nested structures
data = Json parse("{\"users\": [{\"name\": \"Alice\"}]}")
data get("users") at(0) get("name")   // "Alice"

// All JSON types
Json parse("null")      // nil
Json parse("true")      // true
Json parse("42")        // 42
Json parse("\"hello\"") // "hello"
```

## Serialization

```clay
d = Dict clone()
d set("name", "Clay")
d set("version", 1)

Json stringify(d)                // {"name":"Clay","version":1}
Json prettyPrint(d)              // formatted with indentation

// Arrays
Json stringify([1, 2, 3])       // [1,2,3]

// Nested
Json stringify([Dict clone() set("a", 1)])
```

## Methods

| Method | Description |
|--------|-------------|
| `parse(str)` | Parse JSON string into Clay values |
| `stringify(val)` | Serialize Clay value to JSON string |
| `prettyPrint(val)` | Serialize with indentation |

Supported types: `null` ↔ `nil`, `true`/`false` ↔ booleans, numbers, strings, arrays ↔ `Array`, objects ↔ `Dict`.
