# clay-test

A simple, colorful testing framework for the [Clay](https://github.com/mseminatore/clay) programming language.

## Installation

Add to your project's `brick.json`:

```json
{
    "deps": {
        "clay-test": "https://github.com/mseminatore/clay-test.git"
    }
}
```

Then install:

```sh
clay bricks install
```

Or manually copy into your `bricks/` directory.

## Quick Start

```clay
import("clay-test")

Test describe("Math", {
    Test it("adds numbers", {
        Test assertEqual(1 + 1, 2)
    })

    Test it("subtracts numbers", {
        Test assertEqual(10 - 4, 6)
    })
})

Test run()
```

Output:

```
  Math
    ✓ adds numbers
    ✓ subtracts numbers

2 tests, 2 passed, 0 failed
```

## API

### `Test describe(name, block)`

Groups related tests under a named suite. Suites can be nested.

```clay
Test describe("String", {
    Test it("concatenates", {
        Test assertEqual("a" + "b", "ab")
    })
})
```

### `Test it(name, block)`

Defines a single test case. Exceptions are caught and reported as failures.

```clay
Test it("divides", {
    Test assertEqual(10 / 2, 5)
})
```

### `Test assertEqual(actual, expected)`

Asserts that two values are equal. Raises an error with a descriptive message on failure.

### `Test assertNotEqual(actual, expected)`

Asserts that two values are not equal.

### `Test assertTrue(value)`

Asserts that the value is `true`.

### `Test assertFalse(value)`

Asserts that the value is `false`.

### `Test assertRaises(block)`

Asserts that the block raises an error.

```clay
Test it("catches division by zero", {
    Test assertRaises({ 1 / 0 })
})
```

### `Test run()`

Prints the test summary. Call this after all `describe` blocks.

```clay
Test run()
// => 5 tests, 5 passed, 0 failed
```

## Output

- Suite names are displayed in **bold cyan**
- Passing tests show a green **✓** with dimmed test name
- Failing tests show a red **✗** with the test name
- The summary line is green when all tests pass, red when any fail
- Failed tests are listed with their suite, name, and error message

## License

MIT
