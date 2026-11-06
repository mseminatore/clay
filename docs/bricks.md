# Bricks — the package manager

Bricks is Clay's built-in package manager. Packages ("bricks") are Git repositories with a `brick.json` manifest. Any Git URL works — no central registry needed.

## Quick start

```sh
clay bricks init                              # create brick.json
clay bricks add github.com/user/some-lib      # add + install a dependency
clay bricks add github.com/user/clay-foo foo  # ...named (and imported as) "foo"
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

A dependency's name — its key in `deps` — is what you import and the directory it installs into
(`bricks/some-lib`). `clay bricks add` takes the name from the repository unless you give one, so a
repository called `clay-foo` can be imported as plain `foo`.

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
import("stdlib/collections")   // sub-brick inside the stdlib monorepo
import("stdlib/json")          // another sub-brick
```

In `brick.json`, declare a monorepo dependency by appending the sub-brick's path to the repository
URL:

```json
{
  "deps": {
    "stdlib/collections": "github.com/mseminatore/clay-stdlib/collections"
  }
}
```

A dependency's name (the key) is its import path and its directory under `bricks/`. The repository
is cloned once, into the directory named by the part before the first `/` (`bricks/stdlib` here,
whatever the repository itself is called), and every sub-brick from it shares that checkout:
`stdlib/collections` resolves to `bricks/stdlib/collections/brick.json`. Installing fails, rather
than mixing code, if that directory already holds a different repository.

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
| **[stdlib](https://github.com/mseminatore/clay-stdlib)** | Standard library monorepo with `Assert`, `Time`, `MathUtil`, and all bricks below | `github.com/mseminatore/clay-stdlib` |

**stdlib** contains these sub-bricks:

| Sub-brick | Description |
|-----------|-------------|
| **args** | `ArgParser` CLI parsing: `flag`, `option`, `parse`, `usage` |
| **base64** | `Base64` encoding/decoding: `encode`, `decode` with standard alphabet and padding |
| **cache** | `Cache` in-memory LRU/TTL key-value store: `get`, `set`, `has`, `remove`, `clear` |
| **cli** | ANSI terminal colors and styles for CLI output |
| **collections** | Set, Stack, Queue data structures |
| **config** | `Config` INI-style parser: `parse`/`parseFile` into Dict of Dicts, `get` with defaults, `merge` |
| **csv** | `Csv` parse/stringify: `parse` (array of arrays), `parseWithHeader` (array of dicts), `stringify` for CSV generation |
| **enum** | `Enum` named constants with integer value mapping |
| **env** | `Env` environment management: `load` `.env` files, `get` with defaults, `require`, `has` |
| **event** | `Emitter` pub/sub pattern: `on`, `off`, `emit`, `once`, `removeAll`, `listenerCount` |
| **format** | `Fmt` string utilities: `padLeft`, `padRight`, `center`, `repeat`, `capitalize`, `titleCase`, `truncate`, `wrap`, `replaceAll` |
| **functional** | `Fn` utilities: `map`, `filter`, `reduce`, `compose`, `pipe`, `any`, `all`, `find`, `zip`, `flatten`, `sum`, `product` |
| **http-server** | `HttpServer` routing: `get`/`post`/`put`/`delete` routes with `:param` patterns, `text`/`html`/`json`/`redirect` response helpers |
| **iter** | `Iter` lazy pipelines: `from`/`range` sources, `map`/`filter`/`take`/`skip` transforms, `toArray`/`reduce`/`first`/`count`/`each` consumers |
| **json** | `Json` parse/stringify: recursive-descent parser for JSON strings, `stringify`/`prettyPrint` for serialization |
| **log** | `Log` structured logging: `debug`/`info`/`warn`/`error` levels with timestamps, prefixes, and level filtering |
| **markdown** | `Markdown` converter: headings, code blocks, tables, lists, blockquotes to HTML |
| **match** | `Match` pattern matching: chainable `when`/`whenType`/`otherwise` with value and predicate matching |
| **orm** | `Model` ActiveRecord-style ORM: `define`/`create`/`find`/`all`/`where`/`count`/`update`/`delete` over SQLite |
| **path** | `Path` manipulation: `join`, `dirname`, `basename`, `extension`, `stem` |
| **queue** | `JobQueue` SQLite-backed persistent job queue with status tracking |
| **router** | `Router` URL pattern matching: `add`/`match` with `:param` capture, multi-segment patterns |
| **template** | `Template` engine: `render` with `{{key}}` placeholders, `renderEach` for lists |
| **test** | Testing framework: `describe`/`it` blocks, colored output, assertions (`assertEqual`, `assertTrue`, `assertFalse`, `assertRaises`) |
| **url** | `Url` parse/build URLs, `encodeComponent`/`decodeComponent`, `parseQuery` for query strings |
| **uuid** | `Uuid` v4 generation per RFC 4122 |
| **validate** | Schema validation for Dicts with field rules (required, optional, min, max) |

Install the standard library (includes all sub-bricks):

```sh
clay bricks add github.com/mseminatore/clay-stdlib stdlib
```

Then import individual sub-bricks:

```clay norun
import("stdlib/collections")
import("stdlib/json")
```

Or import everything (all of the sub-bricks above):

```clay norun
import("stdlib")
```

---

[← Guide index](README.md)
