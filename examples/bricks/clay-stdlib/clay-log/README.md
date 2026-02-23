# clay-log

Structured logging for Clay with level filtering and timestamps.

## Usage

```clay
import("clay-stdlib/clay-log")
```

## Logging

```clay
Log info("server started")
Log warn("disk space low")
Log error("connection failed")
Log debug("processing request")
```

Output:

```
[INFO] server started
[WARN] disk space low
[ERROR] connection failed
[DEBUG] processing request
```

## Configuration

```clay
// Set minimum level (messages below this level are suppressed)
Log level("warn")
Log debug("hidden")    // suppressed
Log info("hidden")     // suppressed
Log warn("shown")      // printed
Log error("shown")     // printed

// Add a prefix
Log prefix("[app]")
Log info("ready")      // [app] [INFO] ready
```

## Methods

| Method | Description |
|--------|-------------|
| `debug(msg)` | Log at DEBUG level |
| `info(msg)` | Log at INFO level |
| `warn(msg)` | Log at WARN level |
| `error(msg)` | Log at ERROR level |
| `level(name)` | Set minimum level: `"debug"`, `"info"`, `"warn"`, `"error"` |
| `prefix(str)` | Set prefix for all messages |

Level priority: `debug` < `info` < `warn` < `error`. The API is chainable.
