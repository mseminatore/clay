# clay-event

Event emitter (pub/sub) pattern for Clay.

## Usage

```clay
import("clay-stdlib/clay-event")
```

## Example

```clay
emitter = Emitter new()

// Register listeners
emitter on("click", { |data|
    ("Clicked: " + data toString()) println()
})

emitter on("click", { |data|
    ("Also clicked: " + data toString()) println()
})

// Emit events
emitter emitWith("click", "button1")
// Clicked: button1
// Also clicked: button1

// One-time listener
emitter once("ready", { "Ready!" println() })
emitter emit("ready")    // prints "Ready!"
emitter emit("ready")    // nothing (listener removed)
```

## Methods

| Method | Description |
|--------|-------------|
| `new()` | Create a new emitter |
| `on(event, handler)` | Register a listener |
| `off(event)` | Remove all listeners for an event |
| `emit(event)` | Trigger event with no data |
| `emitWith(event, data)` | Trigger event with data |
| `once(event, handler)` | Register a one-time listener |
| `listenerCount(event)` | Number of listeners for an event |
| `events()` | Array of all registered event names |
