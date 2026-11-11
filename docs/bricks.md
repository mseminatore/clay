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
clay bricks search neural networks            # find bricks in the registry
clay bricks info ann                          # what the registry says about one
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

`clay bricks add` and `remove` change only the `"deps"` object. Every other field you wrote
(`license`, `keywords`, the `"native"` section with its per-platform lists), the order and the
formatting stay as they are, so a hand-edited `brick.json` is safe to keep editing. Only
`clay bricks init` writes a whole new file.

A brick can also ship C code in a `"native"` section, compiled the first time it's imported: see
[Native bricks](native-and-plugins.md#native-bricks).

## Using bricks

Import a brick by name (no `.clay` extension, no path separator):

```clay norun
import("some-lib")    // resolves to bricks/some-lib/ using brick.json main
import("utils.clay")  // regular file import (unchanged)
```

Brick resolution: reads `bricks/<name>/brick.json` to find the `main` entry point. Falls back to `bricks/<name>/src/main.clay` if no manifest exists.

`bricks/` is searched in the current directory and up to two parents, then beside the importing
file and up to two of its parents. Last come the bricks bundled with Clay (`stdlib` and `web`, in
the `lib/` directory of the Clay checkout, or in `$CLAY_LIB` if set). A project's own `bricks/` wins,
so `clay bricks add github.com/mseminatore/clay-stdlib@v1.0.0 stdlib` pins one project to a different
standard library without affecting the rest.

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

## Finding bricks

[clay-registry](https://github.com/mseminatore/clay-registry) is a list of bricks, kept for
finding them: one small entry per brick with its name, repository, description and tags. It is
not where bricks live. `clay bricks search <words>` prints the listed bricks whose name,
description or tags contain every word (all of them with no words), and `clay bricks info <name>`
prints everything the list says about one, including the `clay bricks add` line to install it:

```
$ clay bricks search neural
ann  (native: builds C code)
    Neural networks: dense and convolutional layers, training, export and int8 quantization, ...
    https://github.com/mseminatore/clay-ann
```

Searching only reads the list. Nothing is downloaded, built or run until you `clay bricks add` a
URL yourself, and a listing means the entry was checked, not that the code was reviewed: read a
brick before depending on it, and pin it (`@v1.0.0`; `brick.lock` records the commit). The list
is treated as untrusted data: only `https` repositories on known hosts (GitHub, GitLab, Codeberg,
Bitbucket) are shown, and text is clipped. The list is the registry's generated `index.json` (on its `index` branch). The last list fetched is kept in `~/.clay/registry.json`
and used when the network is unavailable. `CLAY_REGISTRY` names another list: a URL, or a local
file for a mirror or a private registry (the same `{"version": 1, "bricks": [...]}` format).

### Native bricks

A brick with a `"native"` section compiles C code with your C compiler the first time it is
imported, and loads it. `clay bricks add` says so when it adds one, and asks before going ahead
when run from a terminal; `--yes` skips the question (for scripts and CI, which also go ahead
without it when there is no terminal to ask). `clay bricks install`, which installs what the
project's own `brick.json` lists, does not ask.

## Publishing a brick

Any Git repository with a `brick.json` is a valid brick. To publish:
1. Add a `brick.json` to your repo (`clay bricks init`)
2. Push to GitHub (or any Git host)
3. Tag releases with semver (`git tag v1.0.0`)
4. Others install with `clay bricks add github.com/you/your-brick@v1.0.0`
5. To let people find it, list it in [clay-registry](https://github.com/mseminatore/clay-registry):
   one JSON file by pull request, as its README describes

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

## Bundled bricks

Two monorepos ship with Clay as git submodules under `lib/` (clone Clay with
`--recurse-submodules`), so they need no `clay bricks install`:

| Brick | Contents | Repo |
|-------|----------|------|
| **stdlib** | The standard library: the sub-bricks below | [github.com/mseminatore/clay-stdlib](https://github.com/mseminatore/clay-stdlib) |
| **web** | `web/http-server`, `web/router`, `web/orm` | [github.com/mseminatore/clay-web](https://github.com/mseminatore/clay-web) |

Import one sub-brick, or a whole monorepo:

```clay norun
import("stdlib/json")
import("stdlib")       // every stdlib sub-brick
import("web/router")
import("web")          // http-server, router and orm
```

**stdlib** contains:

| Sub-brick | Description |
|-----------|-------------|
| **args** | `ArgParser` CLI parsing: `flag`, `option`, `parse`, `usage` |
| **base64** | `Base64` encoding/decoding: `encode`, `decode` with standard alphabet and padding |
| **cache** | `Cache` in-memory LRU/TTL key-value store: `get`, `set`, `has`, `remove`, `clear` |
| **cli** | ANSI terminal colors and styles for CLI output |
| **collections** | Set, Stack, Queue data structures |
| **config** | `Config` INI-style parser: `parse`/`parseFile` into Dict of Dicts, `get` with defaults, `merge` |
| **csv** | `Csv` parse/stringify: `parse` (array of arrays), `parseWithHeader` (array of dicts), `stringify` for CSV generation |
| **diff** | `Diff` line diffs: `lines` (Myers, minimal), `unified` (`diff -u` format), `stats` |
| **enum** | `Enum` named constants with integer value mapping |
| **env** | `Env` environment management: `load` `.env` files, `get` with defaults, `require`, `has` |
| **event** | `Emitter` pub/sub pattern: `on`, `off`, `emit`, `once`, `removeAll`, `listenerCount` |
| **format** | `Fmt` string utilities: `padLeft`, `padRight`, `center`, `repeat`, `capitalize`, `titleCase`, `truncate`, `wrap`, `replaceAll` |
| **fs** | `Fs` file system helpers: `walk`, `glob` (`*`, `?`, `[a-z]`, `**`), `mkdirs`, `copyTree`, `move`, `removeTree`, `withTempDir` |
| **functional** | `Fn` utilities: `map`, `filter`, `reduce`, `compose`, `pipe`, `any`, `all`, `find`, `zip`, `flatten`, `sum`, `product` |
| **iter** | `Iter` lazy pipelines: `from`/`range` sources, `map`/`filter`/`take`/`skip` transforms, `toArray`/`reduce`/`first`/`count`/`each` consumers |
| **json** | `Json` parse/stringify: recursive-descent parser for JSON strings, `stringify`/`prettyPrint` for serialization |
| **log** | `Log` structured logging: `debug`/`info`/`warn`/`error` levels with timestamps, prefixes, and level filtering |
| **markdown** | `Markdown` converter: headings, code blocks, tables, lists, blockquotes to HTML |
| **match** | `Match` pattern matching: chainable `when`/`whenType`/`otherwise` with value and predicate matching |
| **path** | `Path` manipulation: `join`, `dirname`, `basename`, `extension`, `stem` |
| **queue** | `JobQueue` SQLite-backed persistent job queue with status tracking |
| **semver** | `Semver` versions: `parse`, `compare`, `sort`, `max`, `inc`, `satisfies`/`maxSatisfying` with npm ranges (`^1.2`, `~1.2.3`, `>=1 <2`, `\|\|`) |
| **template** | `Template` engine: `render` with `{{key}}` placeholders, `renderEach` for lists |
| **test** | Testing framework: `describe`/`it` blocks, colored output, assertions (`assertEqual`, `assertTrue`, `assertFalse`, `assertRaises`) |
| **toml** | `Toml` TOML 1.0: `parse`, `parseFile` (into Dicts/Arrays), `stringify` |
| **url** | `Url` parse/build URLs, `encodeComponent`/`decodeComponent`, `parseQuery` for query strings |
| **uuid** | `Uuid` v4 generation per RFC 4122 |
| **validate** | Schema validation for Dicts with field rules (required, optional, min, max) |

**web** contains:

| Sub-brick | Description |
|-----------|-------------|
| **http-server** | `HttpServer` routing: `get`/`post`/`put`/`delete` routes with `:param` patterns, `text`/`html`/`json`/`redirect` response helpers |
| **orm** | `Model` ActiveRecord-style ORM: `define`/`create`/`find`/`all`/`where`/`count`/`update`/`delete` over SQLite |
| **router** | `Router` URL pattern matching: `add`/`match` with `:param` capture, multi-segment patterns |

These three were part of stdlib until they moved to clay-web: `import("stdlib/router")` is now
`import("web/router")`.

---

[← Guide index](README.md)
