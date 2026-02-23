# clay-format

String formatting utilities for Clay.

## Usage

```clay
import("clay-stdlib/clay-format")
```

## Methods

All methods are on the `Fmt` object.

```clay
// Padding
Fmt padLeft("hi", 10)                    // "        hi"
Fmt padLeftWith("42", 5, "0")            // "00042"
Fmt padRight("hi", 10)                   // "hi        "
Fmt padRightWith("hi", 10, ".")          // "hi........"
Fmt center("hi", 10)                     // "    hi    "

// String transforms
Fmt capitalize("hello world")            // "Hello world"
Fmt titleCase("hello world")             // "Hello World"
Fmt repeat("ha", 3)                      // "hahaha"
Fmt truncate("hello world", 5)           // "he..."
Fmt replaceAll("aabaa", "a", "x")        // "xxbxx"

// Splitting and joining
Fmt wrap("a long line of text", 10)      // wraps at 10 chars
Fmt join(["a", "b", "c"], ", ")          // "a, b, c"
Fmt lines("a\nb\nc")                     // ["a", "b", "c"]
Fmt chars("abc")                         // ["a", "b", "c"]
```

| Method | Description |
|--------|-------------|
| `padLeft(str, width)` | Right-align with spaces |
| `padLeftWith(str, width, ch)` | Right-align with custom char |
| `padRight(str, width)` | Left-align with spaces |
| `padRightWith(str, width, ch)` | Left-align with custom char |
| `center(str, width)` | Center with spaces |
| `capitalize(str)` | Capitalize first letter |
| `titleCase(str)` | Capitalize each word |
| `repeat(str, n)` | Repeat string n times |
| `truncate(str, maxLen)` | Truncate with "..." |
| `replaceAll(str, old, new)` | Replace all occurrences |
| `wrap(str, width)` | Word-wrap at width |
| `join(arr, sep)` | Join array with separator |
| `lines(str)` | Split on newlines |
| `chars(str)` | Split into characters |
