# clay-collections

Data structures for Clay: **Set**, **Stack**, **Queue**, and **Range**.

## Usage

```clay
import("clay-stdlib/clay-collections")
```

## Set

An unordered collection of unique values.

```clay
s = Set new()
s add(1)
s add(2)
s add(1)           // ignored (duplicate)
s size()            // 2
s contains(1)       // true
s remove(1)
s toArray()         // [2]

// Set operations
a = Set new(); a add(1); a add(2); a add(3)
b = Set new(); b add(2); b add(3); b add(4)
a union(b) toArray()         // [1, 2, 3, 4]
a intersection(b) toArray()  // [2, 3]
a difference(b) toArray()    // [1]
```

| Method | Description |
|--------|-------------|
| `new()` | Create an empty set |
| `add(val)` | Add a value (duplicates ignored) |
| `remove(val)` | Remove a value |
| `contains(val)` | Check membership |
| `size()` | Number of elements |
| `toArray()` | Convert to array |
| `union(other)` | Set union |
| `intersection(other)` | Set intersection |
| `difference(other)` | Set difference |

## Stack

Last-in, first-out (LIFO) collection.

```clay
s = Stack new()
s push(1)
s push(2)
s peek()    // 2 (without removing)
s pop()     // 2
s isEmpty() // false
s size()    // 1
s toArray() // [1]
```

| Method | Description |
|--------|-------------|
| `new()` | Create an empty stack |
| `push(val)` | Push a value |
| `pop()` | Remove and return top |
| `peek()` | Return top without removing |
| `isEmpty()` | Check if empty |
| `size()` | Number of elements |
| `toArray()` | Convert to array |

## Queue

First-in, first-out (FIFO) collection.

```clay
q = Queue new()
q enqueue("a")
q enqueue("b")
q peek()      // "a"
q dequeue()   // "a"
q size()      // 1
q toArray()   // ["b"]
```

| Method | Description |
|--------|-------------|
| `new()` | Create an empty queue |
| `enqueue(val)` | Add to back |
| `dequeue()` | Remove and return front |
| `peek()` | Return front without removing |
| `isEmpty()` | Check if empty |
| `size()` | Number of elements |
| `toArray()` | Convert to array |

## Range

A numeric range with optional step.

```clay
r = Range new(1, 5)
r each({ |n| n println() })   // 1, 2, 3, 4, 5
r map({ |n| n * 2 })          // [2, 4, 6, 8, 10]
r contains(3)                  // true
r size()                       // 5
r toArray()                    // [1, 2, 3, 4, 5]

// Custom step
r2 = Range newWithStep(0, 10, 2)
r2 toArray()                   // [0, 2, 4, 6, 8, 10]
```

| Method | Description |
|--------|-------------|
| `new(start, end)` | Create range (step = 1) |
| `newWithStep(start, end, step)` | Create range with custom step |
| `each(block)` | Iterate over values |
| `map(block)` | Transform values to array |
| `filter(block)` | Filter values to array |
| `contains(n)` | Check if value is in range |
| `size()` | Number of values |
| `toArray()` | Convert to array |
