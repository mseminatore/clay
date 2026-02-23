# clay-iter

Lazy iterator pipelines for Clay.

## Usage

```clay
import("clay-stdlib/clay-iter")
```

## Creating iterators

```clay
// From an array
Iter from([1, 2, 3, 4, 5])

// From a range
Iter range(1, 10)
```

## Transforms

Transforms are lazy -- they don't execute until a consumer is called.

```clay
Iter from([1, 2, 3, 4, 5])
    map({ |x| x * 2 })
    filter({ |x| x > 4 })
    toArray()               // [6, 8, 10]

Iter range(1, 100)
    filter({ |x| x % 2 == 0 })
    take(5)
    toArray()               // [2, 4, 6, 8, 10]

Iter from([1, 2, 3, 4, 5])
    skip(2)
    toArray()               // [3, 4, 5]
```

## Consumers

Consumers execute the pipeline and return a result.

```clay
Iter from([1, 2, 3]) reduce(0, { |acc, x| acc + x })   // 6
Iter from([1, 2, 3]) first()                             // 1
Iter from([1, 2, 3]) count()                             // 3
Iter from([1, 2, 3]) each({ |x| x println() })          // prints 1, 2, 3
```

| Method | Type | Description |
|--------|------|-------------|
| `from(arr)` | Source | Create from array |
| `range(start, end)` | Source | Create from numeric range |
| `map(fn)` | Transform | Apply function to each element |
| `filter(fn)` | Transform | Keep elements matching predicate |
| `take(n)` | Transform | Limit to first n elements |
| `skip(n)` | Transform | Skip first n elements |
| `toArray()` | Consumer | Collect into array |
| `reduce(init, fn)` | Consumer | Fold to single value |
| `first()` | Consumer | Get first element |
| `count()` | Consumer | Count elements |
| `each(fn)` | Consumer | Execute side effect for each |
