# Core objects

The built-in prototypes, starting with a summary table and followed by the
larger ones in detail. Arrays, strings, numbers and Dicts are introduced in the
[language tour](language-tour.md).

| Prototype | Methods |
|-----------|---------|
| Object | `clone()`, `new(args...)`, `toString()`, `println()`, `type()`, `methods()`, `slots()`, `inspect()`, `parent()`, `hasSlot(name)`, `getSlot(name)`, `setSlot(name, val)`, `hasMethod(name)`, `setMethod(name, block)`, `doesNotUnderstand(name, args)` |
| Number | `+`, `-`, `*`, `/`, `%`, `<`, `>`, `<=`, `>=`, `==`, `!=`, `abs()`, `clamp(lo, hi)`, `lerp(to, t)`, `sign()`, `negated()`, `toFloat()`, `toInt()`, `toString()`, `println()`, `times(block)` |
| String | `+`, `==`, `length()`, `at(i)`, `byteAt(i)` (byte value, nil out of range), `chr(code)`, `contains(sub)`, `slice(start, end)`, `split(delim)`, `replace(old, new)`, `toInt()`, `toFloat()`, `trim()`, `upper()`, `lower()`, `startsWith(s)`, `endsWith(s)`, `reverse()`, `indexOf(sub)`, `lastIndexOf(sub)`, `repeat(n)`, `padLeft(n, ch)`, `padRight(n, ch)`, `match(pattern)`, `test(pattern)`, `print()`, `println()`, `toString()` |
| Array | `at(i)`, `setAt(i, val)`, `append(val)`, `push(val)`, `pop()`, `shift()`, `insert(i, val)`, `size()`, `length()`, `each(block)`, `map(block)`, `parallelMap(block)`, `filter(block)`, `reduce(init, block)`, `find(block)`, `sort()`, `reverse()`, `first()`, `last()`, `join(sep)`, `contains(val)`, `indexOf(val)`, `removeAt(i)`, `slice(start, end)`, `flatten()`, `sum()`, `min()`, `max()`, `avg()`, `unique()`, `==`, `!=`, `toString()` |
| Block | `value(args...)`, `whileTrue { block }`, `whileFalse { block }` |
| Boolean | `ifTrue { block }`, `ifFalse { block }`, `not()`, `toString()` |
| Error | `message()`, `type()`, `line()`, `file()`, `stackTrace()`, `toString()` |
| Dict | `set(key, val)`, `get(key)`, `has(key)`, `remove(key)`, `size()`, `keys()`, `values()`, `each(block)`, `==`, `!=`, `toString()` (entries keep insertion order) |
| Math | `abs(n)`, `sqrt(n)`, `pow(b,e)`, `min(a,b)`, `max(a,b)`, `clamp(n, lo, hi)`, `lerp(a, b, t)`, `sign(n)`, `floor(n)`, `ceil(n)`, `round(n)`, `sin(n)`, `cos(n)`, `random()`, `pi`, `e` |
| Console | `print(args...)`, `println(args...)`, `error(args...)` (to stderr), `isTerminal()` (output is a terminal, so colour codes are safe), `readLine()`, `readLine(prompt)` (on a terminal: shows the prompt, with line editing and history), `loadHistory(path)`, `saveHistory(path)`, `prompt(msg)` |
| File | `read(path)`, `readLines(path)`, `write(path, content)`, `append(path, content)`, `exists(path)`, `delete(path)`, `rename(old, new)`, `size(path)`, `isDir(path)`, `isFile(path)`, `mkdir(path)`, `listDir(path)` (sorted), `copy(from, to)`, `modified(path)`, `cwd()`, `chdir(path)`, `tempDir()` — binary-safe; `delete` also removes empty directories. Recursive helpers are in `stdlib/fs` |
| System | `exit(code)`, `run(command)` (shell command → exit status; use `Process` to capture output), `env(name)`, `args()`, `executable()`, `pluginGlobals()`, `platform()`, `version()` (the Clay version, e.g. `"0.1.0"`), `clock()`, `allocCount()`, `deallocCount()`, `liveObjects()` (allocated − freed), `memory()` (process resident memory in bytes) |
| FFI | `load(path)`, `call(funcName, args...)`, `close()` |
| Date | `now()`, `fromTimestamp(ts)`, `year()`, `month()`, `day()`, `hour()`, `minute()`, `second()`, `weekday()`, `timestamp()`, `format(pattern)`, `toString()` |
| Http | `get(url)`, `post(url, body)`, `serve(port, handler)`, `response(status, body)`, `jsonResponse(status, body)`, `parseForm(str)`, `status()`, `body()`, `method()`, `path()`, `query()`, `params()` |
| Regex | `match(str, pattern)`, `test(str, pattern)`, `replace(str, pattern, sub)`, `find(str, pattern)` |
| Crypto | `sha256(str)`, `sha1(str)`, `md5(str)`, `hmacSha256(key, data)` (hex digests), `randomBytes(n)` (hex, from the OS), `constantTimeEquals(a, b)` |
| Process | `run(cmd, options)` → `stdout`, `stderr`, `exitCode`, `ok`; `stream(cmd, options) { \|line\| ... }` → exit code. `cmd` is an argv array, or a string with `#{"shell": true}`; options `shell`, `cwd`, `input` |
| Fiber | `new(block)`, `resume()`, `isDone()`, `yield(value)` |
| Db | `open(path)`, `exec(sql)`, `query(sql)`, `close()`, `escape(str)`, `lastInsertId()` |
| Range | `each(block)`, `map(block)`, `filter(block)`, `contains(val)`, `toArray()`, `size()`, `step(n)`, `reverse()`, `start()`, `end()`, `toString()` |
| Vec3 | `new(x,y,z)`, `x`, `y`, `z`, `+`, `-`, `*`, `/`, `dot(v)`, `cross(v)`, `length`, `lengthSquared`, `normalize`, `mul(v)`, `reflect(n)`, `nearZero`, `negated`, `==`, `!=`, `toString()` |
| Thread | `spawn(block)`, `await()`, `done()`, `available` |
| VM | `run(unit)`, `run(unit, args)`, `hasGlobal(name)`, `counts()` — run bytecode from the [Clay compiler in Clay](native-and-plugins.md#a-clay-compiler-in-clay) (native programs only) |
| Graphics | `window(w,h,title)`, `close`, `clear`, `color(r,g,b)`, `setPixel(x,y,r,g,b)`, `drawLine(x1,y1,x2,y2)`, `drawRect(x,y,w,h)`, `fillRect(x,y,w,h)`, `drawCircle(x,y,r)`, `fillCircle(x,y,r)`, `drawText(x,y,text)`, `fontSize(n)`, `loadPPM(path)`, `drawImage(img,x,y)`, `drawImageEx(img,x,y,scale)`, `createSprite(img,x,y,w,h)`, `drawSprite(s,x,y)`, `createBuffer(w,h)`, `bufferPixel(buf,x,y,r,g,b)`, `drawBuffer(buf,x,y)`, `pollEvent`, `waitEvent`, `isKeyDown(key)`, `mouseX`, `mouseY`, `mouseDown`, `ticks`, `delay(ms)`, `fps`, `refresh`, `resize(w,h)`, `fullscreen`, `windowed`, `width`, `height` |

## System

The `System` object provides OS-level operations:

```clay
System platform             // "windows", "macos", or "linux"
System env("PATH")            // environment variable (nil if unset)
System args                 // command-line arguments as array
System clock                // monotonic clock in seconds (float)
System exit(0)                // exit process with code

// Memory diagnostics
System allocCount           // total objects allocated
System deallocCount         // total objects freed
```

## Running programs

`Process run` runs a program, waits for it, and returns its output and exit code:

```clay norun
r = Process run(["git", "log", "--oneline", "-5"])
r ok          // true if it exited with 0
r exitCode    // 0
r stdout      // the program's output, as one string
r stderr      // its error output
```

Pass the command as an **array**: the first element is the program (found on
`PATH`) and each other element is one argument, passed exactly as written. No
shell is involved, so spaces, quotes, `$` and `*` in arguments need no escaping
and can't be misread as shell syntax — the safe way to pass user input or file
names.

To use shell features such as pipes, redirection or wildcards, pass a string
and ask for the shell (`/bin/sh -c`, or `cmd.exe /c` on Windows):

```clay
r = Process run("echo hello | tr a-z A-Z", #{"shell": true})
r stdout trim    // "HELLO" (on Unix)
```

Options, all optional: `shell` (run a string command through the shell), `cwd`
(the directory to run in) and `input` (a string written to the program's
stdin; otherwise the program shares Clay's stdin).

```clay norun
Process run(["sort"], #{"input": "pear\napple\n"}) stdout   // "apple\npear\n"
Process run(["make"], #{"cwd": "build"})
```

`Process stream` is for long-running programs: it calls the block with each
line of output as it arrives (without the line ending), leaves error output on
the terminal, and returns the exit code. `break` in the block stops the
program early (and `stream` returns `nil`); an error raised in the block stops
it and propagates.

```clay norun
Process stream(["ping", "-c", "3", "example.com"]) { |line| ("> " + line) println }
```

A program that can't be started (not found, not executable, or a bad `cwd`)
raises `RuntimeError`. A program killed by a signal reports an exit code of
128 + the signal number, as shells do.

## File operations

```clay
// Write / append / read
File write("notes.txt", "hello")
File append("notes.txt", "\nworld")
File read("notes.txt") println                // hello (newline) world
(File readLines("notes.txt") size) println    // 2

// Query
File exists("notes.txt")             // true
File size("notes.txt")               // 11 (bytes)
File isFile("notes.txt")             // true

// Directories, rename, delete
(File isDir("output")) ifFalse { File mkdir("output") }
File rename("notes.txt", "output/notes.txt")
File listDir("output") println       // [notes.txt] (always sorted)
File copy("output/notes.txt", "output/backup.txt")
File delete("output/notes.txt")
File delete("output/backup.txt")
File delete("output")                // a directory, once it is empty

// Times, working directory, scratch space
File write("stamp.txt", "x")
File modified("stamp.txt")           // seconds since 1970 (Date fromTimestamp(...))
File delete("stamp.txt")
File cwd                             // the current directory
tmp = File tempDir                   // a new empty directory under the system temp dir
File delete(tmp)
```

Files are read and written as raw bytes, so binary data (images, archives)
round-trips exactly, and text is written with the line endings it contains.
For recursive work — walking a tree, globbing, copying or removing whole
directories — use [`stdlib/fs`](bricks.md#bundled-bricks):

```clay norun
import("stdlib/fs")
Fs glob("src/**/*.clay") each { |path| path println }
Fs removeTree("build")
```

## Date and time

```clay
d = Date now
d toString println               // "2026-02-23 14:30:45"
d year                             // 2026
d month                            // 2 (1-12)
d day                              // 23 (1-31)
d hour                             // 14 (0-23)
d minute                           // 30 (0-59)
d second                           // 45 (0-59)
d weekday                          // 1 (0=Sun..6=Sat)
d timestamp                        // Unix epoch seconds

// Create from timestamp
d2 = Date fromTimestamp(1700000000)

// strftime-style formatting
d format("%A, %B %d, %Y") println  // "Sunday, February 23, 2026"
d format("%H:%M") println          // "14:30"
```

## HTTP client

```clay norun
// GET request
resp = Http get("http://example.com")
resp status println              // 200
resp body println                // HTML content

// HTTPS (Windows always, Unix/macOS with OpenSSL)
resp = Http get("https://example.com")

// POST request
resp = Http post("http://api.example.com/data", "hello")
resp status println
resp body println
```

> **Note:** On Windows, Http uses WinHTTP and supports HTTPS natively. On Unix/macOS, HTTPS requires OpenSSL (`brew install openssl` on macOS, typically pre-installed on Linux). Build with `cmake` and it will auto-detect OpenSSL. Without OpenSSL, only HTTP is supported.

## HTTP server

```clay norun
// Simple server — handle all requests with a single block
Http serve(8080, { |req|
    method = req method   // "GET", "POST", etc.
    path = req path       // "/api/data"
    query = req query     // "name=clay" (after ?)
    body = req body       // POST body

    Http response(200, "Hello World")
})

// JSON response
Http serve(8080, { |req|
    Http jsonResponse(200, "{\"status\": \"ok\"}")
})
```

For routing, use the `clay-http-server` brick:

```clay norun
import("web/http-server")

server = HttpServer new(8080)
server get("/", { |req| server html(200, "<h1>Hello!</h1>") })
server get("/api/time", { |req| server json(200, Date now toString) })
server post("/api/echo", { |req| server text(200, req body) })

// URL params with :param patterns
server get("/users/:id", { |req|
    id = req params get("id")
    server json(200, "{\"user\": " + id + "}")
})

// Redirect helper
server post("/login", { |req| server redirect("/dashboard") })
server start
```

## Regular expressions

```clay
// Match first occurrence
Regex match("hello world", "\\w+")           // "hello"

// Test if pattern matches
Regex test("abc123", "\\d+")                 // true

// Replace all matches
Regex replace("a1b2c3", "\\d", "X")          // "aXbXcX"

// Find all matches
Regex find("a1 b2 c3", "\\d+")              // ["1", "2", "3"]

// String convenience methods
"hello world" match("\\w+")                  // "hello"
"abc123" test("[0-9]+")                      // true
```

Supports ERE (Extended Regular Expressions): `.`, `^`, `$`, `*`, `+`, `?`, `[...]`, `[^...]`, `\d`, `\w`, `\s`, `(...)`, `|`, `\\`.

## Hashing and secure random

```clay
Crypto sha256("hello")       // "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
Crypto hmacSha256("secret", "message")   // hex HMAC-SHA256, e.g. to sign a cookie
Crypto randomBytes(16)       // 32 hex characters: a session token, salt or nonce

// Compare a received signature without leaking timing information
sig = Crypto hmacSha256("secret", "message")
Crypto constantTimeEquals(sig, Crypto hmacSha256("secret", "message"))   // true
```

Digests are lowercase hex strings. `randomBytes` reads the operating system's secure random
number generator (`/dev/urandom`, or `BCryptGenRandom` on Windows) and raises `RuntimeError` if it
is unavailable rather than falling back to a weaker source. `sha1` and `md5` are for checksums and
older protocols; use `sha256` for anything security-related. Base64 lives in the standard library
(`import("stdlib/base64")`).

## Fibers (coroutines)

```clay
// Create a fiber from a block
f = Fiber new {
    Fiber yield(1)
    Fiber yield(2)
    3
}

f resume     // 1
f resume     // 2
f resume     // 3 (final value)
f isDone     // true

// Generator pattern — yield from inside loops
counter = Fiber new {
    i = 0
    { i < 5 } whileTrue {
        Fiber yield(i)
        i = i + 1
    }
}
counter resume   // 0
counter resume   // 1
counter resume   // 2

// Yield from array iteration
items = Fiber new {
    [10, 20, 30] each { |x| Fiber yield(x) }
}
items resume     // 10
items resume     // 20

// Two independent fibers
a = Fiber new { Fiber yield(10); 20 }
b = Fiber new { Fiber yield(30); 40 }
a resume     // 10
b resume     // 30
a resume     // 20
b resume     // 40
```

## Vec3

A built-in 3D vector type optimized for math-heavy code (ray tracing, physics, etc.).

```clay
// Create vectors
v = Vec3 new(1.0, 2.0, 3.0)
v x   // 1.0
v y   // 2.0
v z   // 3.0

// Arithmetic (scalar and vector)
a = Vec3 new(1, 2, 3)
b = Vec3 new(4, 5, 6)
a + b              // Vec3(5, 7, 9)
a - b              // Vec3(-3, -3, -3)
a * 2.0            // Vec3(2, 4, 6)
a / 2.0            // Vec3(0.5, 1, 1.5)
a mul(b)           // Vec3(4, 10, 18)  — component-wise

// Geometry
a dot(b)           // 32.0
a cross(b)         // Vec3(-3, 6, -3)
a length           // 3.7416...
a lengthSquared    // 14
a normalize        // unit vector
a negated          // Vec3(-1, -2, -3)
a nearZero         // false

// Reflection (for ray tracing)
incoming = Vec3 new(1, -1, 0) normalize
normal = Vec3 new(0, 1, 0)
incoming reflect(normal)   // reflected direction
```

## Threads

OS-level threads for CPU-bound parallelism. Requires building with `-DCLAY_THREAD_SAFE=ON`.

```clay
// Check if threading is available at runtime
Thread available println   // true (thread-safe build) or false

// Spawn a thread — runs a block on a new OS thread
Thread available ifTrue {
    t = Thread spawn { 2 + 3 }
    t await println        // 5 — blocks until the thread completes
    t done println         // true
}

// parallelMap — parallel map over an array. Auto-detects the CPU count and
// splits the work across threads; falls back to a sequential map when
// threading is unavailable, so this works in every build
results = [1, 2, 3, 4, 5] parallelMap { |x| x * x }
results println            // [1, 4, 9, 16, 25]
```

Each element can be a substantial task — the parallel ray tracer
(`examples/raytrace_parallel.clay`) renders one scanline per element:

```clay norun
image = scanlines parallelMap { |args|
    renderScanline value(args)    // runs on a worker thread
}
```

Building with thread support:

```sh
cmake -B build -DCLAY_THREAD_SAFE=ON
cmake --build build --config Release
```

## Graphics

SDL2-based graphics for windows, drawing, sprites, and input. Automatically enabled when SDL2 is found by CMake (install via `vcpkg install sdl2`).

```clay norun
// Open a window
Graphics window(800, 600, "My App")

// Drawing primitives
Graphics clear
Graphics color(255, 0, 0)           // set draw color (RGB)
Graphics drawLine(0, 0, 100, 100)
Graphics drawRect(50, 50, 200, 150)
Graphics fillRect(50, 50, 200, 150)
Graphics drawCircle(400, 300, 50)
Graphics fillCircle(400, 300, 50)
Graphics setPixel(10, 20, 255, 128, 0)   // x, y, r, g, b
Graphics refresh                          // present to screen

// Text
Graphics fontSize(24)
Graphics drawText(10, 10, "Hello!")

// Images (PPM format)
img = Graphics loadPPM("image.ppm")
Graphics drawImage(img, 0, 0)
Graphics drawImageEx(img, 0, 0, 2.0)   // with scale

// Sprites (sub-image regions from a source image)
sprite = Graphics createSprite(img, 0, 0, 32, 32)   // x, y, w, h
Graphics drawSprite(sprite, 100, 100)
Graphics drawSpriteEx(sprite, 100, 100, 2.0, 0, false, false)  // scale, angle, flipX, flipY

// Offscreen buffers
buf = Graphics createBuffer(320, 240)
Graphics bufferPixel(buf, 10, 10, 255, 0, 0)
Graphics drawBuffer(buf, 0, 0)

// Event handling
evt = Graphics pollEvent       // non-blocking, returns Dict or nil
evt = Graphics waitEvent       // blocking
(evt != nil) ifTrue {
    evt get("type")            // "quit", "keydown", "keyup", "mousedown", "mousemove", etc.
    evt get("key")             // key name (for key events)
    evt get("x")               // mouse x (for mouse events)
}

// Input queries
Graphics isKeyDown("Space")   // true/false
Graphics mouseX               // current mouse x
Graphics mouseY               // current mouse y
Graphics mouseDown             // true if any mouse button pressed

// Timing
Graphics ticks                 // milliseconds since SDL init
Graphics delay(16)             // sleep ~16ms (for frame limiting)
Graphics fps                   // frames per second

// Window management
Graphics width                 // window width
Graphics height                // window height
Graphics resize(1024, 768)
Graphics fullscreen
Graphics windowed
Graphics close
```

## Database (SQLite)

```clay
// Open (or create) a database
db = Db open("app.db")

// Create a table
db exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)")

// Insert data
db exec("INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')")
db exec("INSERT INTO users (name, email) VALUES ('Bob', 'bob@example.com')")

// Query — returns an Array of Dicts
rows = db query("SELECT id, name, email FROM users")
rows each { |row|
    (row get("name") + ": " + row get("email")) println
}
// Alice: alice@example.com
// Bob: bob@example.com

// Last insert ID
db exec("INSERT INTO users (name, email) VALUES ('Carol', 'carol@example.com')")
db lastInsertId   // 3

// Escape strings for safe SQL (doubles single quotes)
db escape("O'Brien")   // "O''Brien"

// Float columns are returned as Clay floats
db exec("CREATE TABLE prices (id INTEGER PRIMARY KEY, amount REAL)")
db exec("INSERT INTO prices (amount) VALUES (9.99)")
db query("SELECT amount FROM prices") at(0) get("amount")   // 9.99

// Close when done
db close
```

---

[← Guide index](README.md)
