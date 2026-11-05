# Bricks — the package manager

Bricks is Clay's built-in package manager. Packages ("bricks") are Git repositories with a `brick.json` manifest. Any Git URL works — no central registry needed.

## Quick start

```sh
clay bricks init                              # create brick.json
clay bricks add github.com/user/some-lib      # add + install a dependency
clay bricks install                           # install all deps from brick.json
clay bricks update                            # update all deps to latest
clay bricks list                              # show project info
clay bricks remove some-lib                   # remove a dependency
```

## brick.json

```json
{
  "name": "my-project",
  "version": "0.1.0",
  "description": "A Clay project",
  "main": "src/main.clay",
  "deps": {
    "some-lib": "github.com/user/some-lib@v1.0"
  }
}
```

Dependencies are pinned to a Git tag, branch, or commit via `@ref`. URLs are automatically prefixed with `https://` if no protocol is specified.

## Using bricks

Import a brick by name (no `.clay` extension, no path separator):

```clay norun
import("some-lib")    // resolves to bricks/some-lib/ using brick.json main
import("utils.clay")  // regular file import (unchanged)
```

Brick resolution: reads `bricks/<name>/brick.json` to find the `main` entry point. Falls back to `bricks/<name>/src/main.clay` if no manifest exists.

## Monorepo bricks

A single Git repository can contain multiple bricks (a "monorepo"). Import sub-bricks using slash-separated paths:

```clay norun
import("clay-stdlib/clay-collections")   // sub-brick inside clay-stdlib
import("clay-stdlib/clay-json")          // another sub-brick
```

In `brick.json`, declare monorepo dependencies with `#sub-brick` syntax:

```json
{
  "deps": {
    "clay-stdlib/clay-collections": "github.com/mseminatore/clay-stdlib#clay-collections"
  }
}
```

Sub-bricks resolve to `bricks/<repo>/<sub-brick>/brick.json`.

## Publishing a brick

Any Git repository with a `brick.json` is a valid brick. To publish:
1. Add a `brick.json` to your repo (`clay bricks init`)
2. Push to GitHub (or any Git host)
3. Tag releases with semver (`git tag v1.0.0`)
4. Others install with `clay bricks add github.com/you/your-brick@v1.0.0`

## Local layout

```
my-project/
  brick.json              # project manifest
  brick.lock              # exact commits installed (commit this)
  src/main.clay            # your code
  bricks/                  # dependencies (gitignore this)
    some-lib/
      brick.json
      src/main.clay
```

The `bricks/` directory should be gitignored — it's recreated by `clay bricks install`.

`brick.lock` records the exact commit of every dependency. Commit it: with a
matching lock file, `clay bricks install` checks out those commits, so everyone
(and CI) builds against the same code even when a dependency's branch moves on.
`clay bricks update` fetches newer commits and rewrites the lock file.

## Available bricks

The standard library is available at:

| Brick | Description | Repo |
|-------|-------------|------|
| **[clay-stdlib](https://github.com/mseminatore/clay-stdlib)** | Standard library monorepo with `Assert`, `Time`, `MathUtil`, and all bricks below | `github.com/mseminatore/clay-stdlib` |

**clay-stdlib** contains these sub-bricks:

| Sub-brick | Description |
|-----------|-------------|
| **clay-args** | `ArgParser` CLI parsing: `flag`, `option`, `parse`, `usage` |
| **clay-base64** | `Base64` encoding/decoding: `encode`, `decode` with standard alphabet and padding |
| **clay-cache** | `Cache` in-memory LRU/TTL key-value store: `get`, `set`, `has`, `remove`, `clear` |
| **clay-cli** | ANSI terminal colors and styles for CLI output |
| **clay-collections** | Set, Stack, Queue, Range data structures |
| **clay-config** | `Config` INI-style parser: `parse`/`parseFile` into Dict of Dicts, `get` with defaults, `merge` |
| **clay-csv** | `Csv` parse/stringify: `parse` (array of arrays), `parseWithHeader` (array of dicts), `stringify` for CSV generation |
| **clay-enum** | `Enum` named constants with integer value mapping |
| **clay-env** | `Env` environment management: `load` `.env` files, `get` with defaults, `require`, `has` |
| **clay-event** | `Emitter` pub/sub pattern: `on`, `off`, `emit`, `once`, `removeAll`, `listenerCount` |
| **clay-format** | `Fmt` string utilities: `padLeft`, `padRight`, `center`, `repeat`, `capitalize`, `titleCase`, `truncate`, `wrap`, `replaceAll` |
| **clay-functional** | `Fn` utilities: `map`, `filter`, `reduce`, `compose`, `pipe`, `any`, `all`, `find`, `zip`, `flatten`, `sum`, `product` |
| **clay-http-server** | `HttpServer` routing: `get`/`post`/`put`/`delete` routes with `:param` patterns, `text`/`html`/`json`/`redirect` response helpers |
| **clay-iter** | `Iter` lazy pipelines: `from`/`range` sources, `map`/`filter`/`take`/`skip` transforms, `toArray`/`reduce`/`first`/`count`/`each` consumers |
| **clay-json** | `Json` parse/stringify: recursive-descent parser for JSON strings, `stringify`/`prettyPrint` for serialization |
| **clay-log** | `Log` structured logging: `debug`/`info`/`warn`/`error` levels with timestamps, prefixes, and level filtering |
| **clay-markdown** | `Markdown` converter: headings, code blocks, tables, lists, blockquotes to HTML |
| **clay-match** | `Match` pattern matching: chainable `when`/`whenType`/`otherwise` with value and predicate matching |
| **clay-orm** | `Model` ActiveRecord-style ORM: `define`/`create`/`find`/`all`/`where`/`count`/`update`/`delete` over SQLite |
| **clay-path** | `Path` manipulation: `join`, `dirname`, `basename`, `extension`, `stem` |
| **clay-queue** | `Queue` SQLite-backed persistent job queue with status tracking |
| **clay-router** | `Router` URL pattern matching: `add`/`match` with `:param` capture, multi-segment patterns |
| **clay-template** | `Template` engine: `render` with `{{key}}` placeholders, `renderEach` for lists |
| **clay-test** | Testing framework: `describe`/`it` blocks, colored output, assertions (`assertEqual`, `assertTrue`, `assertFalse`, `assertRaises`) |
| **clay-url** | `Url` parse/build URLs, `encodeComponent`/`decodeComponent`, `parseQuery` for query strings |
| **clay-uuid** | `Uuid` v4 generation per RFC 4122 |
| **clay-validate** | Schema validation for Dicts with field rules (required, optional, min, max) |

Install the standard library (includes all sub-bricks):

```sh
clay bricks add github.com/mseminatore/clay-stdlib
```

Then import individual sub-bricks:

```clay norun
import("clay-stdlib/clay-collections")
import("clay-stdlib/clay-json")
```

Or import everything:

```clay norun
import("clay-stdlib")
```

---

[← Guide index](README.md)
