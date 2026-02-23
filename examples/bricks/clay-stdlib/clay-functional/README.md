# clay-functional

Functional programming utilities for Clay.

## Usage

```clay
import("clay-stdlib/clay-functional")
```

## Methods

All methods are on the `Fn` object.

```clay
// Transform
Fn map([1, 2, 3], { |x| x * 2 })                    // [2, 4, 6]
Fn filter([1, 2, 3, 4], { |x| x > 2 })               // [3, 4]
Fn reduce([1, 2, 3], 0, { |acc, x| acc + x })         // 6

// Search
Fn find([1, 2, 3], { |x| x > 1 })                    // 2
Fn count([1, 2, 3, 4], { |x| x > 2 })                // 2

// Predicates
Fn any([1, 2, 3], { |x| x > 2 })                     // true
Fn all([1, 2, 3], { |x| x > 0 })                     // true
Fn none([1, 2, 3], { |x| x > 5 })                    // true

// Composition
double = { |x| x * 2 }
inc = { |x| x + 1 }
Fn compose(double, inc) value(3)                      // 8 (double(inc(3)))
Fn pipe([inc, double]) value(3)                       // 8 (inc then double)

// Utilities
Fn flatten([[1, 2], [3, 4]])                          // [1, 2, 3, 4]
Fn zip([1, 2], ["a", "b"])                            // [[1, "a"], [2, "b"]]
Fn sum([1, 2, 3])                                     // 6
Fn product([1, 2, 3])                                 // 6
```

| Method | Description |
|--------|-------------|
| `map(arr, block)` | Transform each element |
| `filter(arr, block)` | Keep elements matching predicate |
| `reduce(arr, init, block)` | Fold to single value |
| `find(arr, block)` | First element matching predicate |
| `count(arr, block)` | Count matching elements |
| `any(arr, block)` | True if any element matches |
| `all(arr, block)` | True if all elements match |
| `none(arr, block)` | True if no elements match |
| `compose(f, g)` | Function composition (f after g) |
| `pipe(fns)` | Pipeline (left to right) |
| `flatten(arr)` | Flatten nested arrays |
| `zip(a, b)` | Pair elements from two arrays |
| `sum(arr)` | Sum of numeric array |
| `product(arr)` | Product of numeric array |
