# Plugin API reference

A plugin adds prototypes and methods written in C. It talks to Clay only through the table of
functions in [`src/runtime/clay_plugin.h`](../src/runtime/clay_plugin.h), `ClayPluginAPI`, so it
doesn't depend on Clay's internals and keeps working as they change. The same C serves a plugin
loaded at run time (`System plugin`, the `plugins/` folder) and a [native
brick](native-and-plugins.md#native-bricks), which Clay compiles for you.

This page is the reference. For a first plugin, building it and loading it, see [Native
plugins](native-and-plugins.md#native-plugins); `clay bricks new my-brick --native` writes a
working one to start from.

- [A plugin in outline](#a-plugin-in-outline)
- [Values and ownership](#values-and-ownership)
- [Errors](#errors)
- [Function reference](#function-reference)
- [Native data: wrapping a C struct](#native-data-wrapping-a-c-struct)
- [Loading](#loading)
- [Versions](#versions)

## A plugin in outline

```c
#include "clay_plugin.h"

static const ClayPluginAPI *api;      /* keep the table: every call goes through it */

static ClayPluginValue greet(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);
    if (argc < 1 || !api->is_string(argv[0])) {
        api->raise("greet expects a String", "ArgumentError");
        return CLAY_NIL;
    }
    api->write_output("hello, ");
    api->write_output(api->to_string(argv[0]));
    api->write_output("\n");
    return CLAY_NIL;
}

static ClayPluginMethod methods[] = { { "greet", greet }, { NULL, NULL } };

CLAY_PLUGIN_EXPORT void hello_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "Hello", methods, NULL, 0 };
    api->register_type(&def);         /* a global named Hello */
}
```

```clay norun
Hello = System plugin("hello")
Hello greet("world")
```

- **The entry point** is `<name>_plugin_init`, where `<name>` is the library's file name without
  its extension (`hello.so` → `hello_plugin_init`; a native brick's `"plugin"`). The name is
  unique, so several plugins can be linked into one executable. `clay_plugin_init` is still
  accepted from a shared library. Mark the entry point `CLAY_PLUGIN_EXPORT`.
- **A method** is `ClayPluginValue fn(ClayPluginObject self, int argc, ClayPluginValue *argv)`.
  `self` is the receiver, `argv` holds `argc` arguments. Clay pads `argv` with nil up to at
  least 4 entries, but check `argc` and each argument's type before you use it.
- **`api`** is the same table for the life of the program. Keep it in a static, as above.
- **Output** goes through `api->write_output`, not `printf`. Under the debugger (`clay --dap`)
  stdout carries the debug protocol, and text printed there is lost.

## Values and ownership

A `ClayPluginValue` is any Clay value: nil, a number, a String, an object. A `ClayPluginObject`
is an object, as `self` is and as `register_type` and `clone` return. One can be passed where
the other is expected by a cast: `(ClayPluginValue)obj`.

`CLAY_NIL` is nil (equal to `api->make_nil()`). Booleans are `make_bool(1)` / `make_bool(0)`.

Clay counts references. A plugin follows two rules:

1. **A value you get as a new reference is yours to give away or release.** These are new
   references: the results of every `make_*` function, `send`, `try_send` (both results),
   `array_new`, `from_floats` and `dict_new`. Return one from the method (the caller takes it), store
   it somewhere that keeps it (`array_push`, `dict_set` and `set_slot` take their own
   reference, so release yours afterwards), or `release` it.
2. **A value you are handed is borrowed.** Arguments (`argv`), `self`, and the results of
   `get_slot`, `array_get`, `dict_get` and `dict_next` stay valid while their holder keeps them,
   which is at least until the method returns. To keep one longer (in native data, in a
   static), `retain` it, and `release` it when you are done.

Returning `self` or one of the arguments is fine: a method's result may alias its receiver or
arguments, and Clay takes its own reference.

```c
ClayPluginValue out = api->array_new();          /* new: ours */
ClayPluginValue name = api->make_string("x");    /* new: ours */
api->array_push(out, name);                      /* the array retains it */
api->release(name);                              /* so drop ours */
api->array_push(out, api->make_int(3));          /* numbers aren't counted: no release needed */
return out;                                      /* the caller takes ours */
```

Numbers (`make_int`, `make_float`), nil and booleans aren't counted; releasing one does nothing.
So `api->array_push(arr, api->make_int(n))` is fine as it stands, but
`api->array_push(arr, api->make_string(s))` leaks the string.

## Errors

`api->raise(message, type)` raises an error: `try { } catch { |e| }` in Clay catches it, with `e
message` the message and `e type` the type. The type can be any name. The usual ones are
`ArgumentError` (a bad argument), `TypeError` (a value of the wrong kind) and `RuntimeError`.

**An error does not return to your code.** Clay unwinds with `longjmp`, straight past the C
frames of the plugin. That is true of `raise`, and of any error raised inside a call back into
Clay: `send`, `to_floats` (on a non-number), `make_int` (out of range), `clone` (out of memory),
`to_int` (a float out of range). The code after the call doesn't run, and neither does the rest
of the method. So:

- **Check arguments before you allocate.** Validate first, then `malloc`.
- **Don't hold anything across a call that can raise** that only the code after it would free:
  a `malloc`'d buffer, an open file, a lock, a new reference. Put memory in native data (freed
  with the object), or take control back with `try_send`.
- After `raise`, `return CLAY_NIL;` anyway: the compiler needs a return value, and it is never
  reached.

### Getting control back: `try_send` (version 6)

```c
int try_send(ClayPluginValue recv, const char *sel, int argc,
             ClayPluginValue *argv, ClayPluginValue *out);
```

`try_send` sends like `send`, but catches what the method raises. It returns 1 with the result in
`*out`, or 0 with the raised value in `*out`, both new references. After a 0, clean up, then
either pass the error on with `raise_value(*out)` (which takes the reference) or handle it and
`release` it.

```c
char *buf = malloc(n);
ClayPluginValue r;
if (!api->try_send(callback, "value", 1, &arg, &r)) {
    free(buf);
    api->raise_value(r);          /* the caller sees the original error */
}
/* ... use r and buf ... */
```

Only an error should be swallowed. `break`, `continue` and a non-local `return` in a Clay block
travel the same way: `try_send(block, "value", ...)` on a block whose code runs `return 5` comes
back with 0, and the return has to reach its method. `is_error(v)` tells them apart: release
`v` only when `is_error(v)` is 1, otherwise `raise_value(v)`.

```c
if (!api->try_send(recv, "load", 0, NULL, &r)) {
    cleanup();
    if (!api->is_error(r)) api->raise_value(r);     /* break / return: let it go on */
    api->release(r);                                /* an error: handled */
    r = CLAY_NIL;
}
```

## Function reference

The version column is the `api_version` that added the function. Check `api->api_version` before
using anything above 1 if your plugin has to load into an older Clay. A version-1 Clay has no
`api_version` field at all, so a plugin that supports it can't check; every current Clay passes
the newest table.

### Registering

| Function | Since | |
|---|---|---|
| `ClayPluginObject register_type(const ClayPluginType *def)` | 1 | Makes a prototype, cloned from `Object`, with `def->methods`, and a global named `def->name`. Returns the prototype. |
| `void register_methods(ClayPluginObject proto, const ClayPluginMethod *methods)` | 1 | Adds methods (a `{ NULL, NULL }`-terminated array) to `proto`, replacing any with the same name. |

```c
typedef struct {
    const char       *name;       /* the global's name, and the prototype's `type` */
    ClayPluginMethod *methods;    /* { name, fn } pairs ending with { NULL, NULL }, or NULL */
    void (*free_fn)(void *data);  /* optional: runs on an object's native data when it's freed */
    size_t data_size;             /* optional: a native data buffer for each clone (version 6) */
} ClayPluginType;
```

`register_type` copies what it needs from `def`, so `def` can be a local variable. The method
names and functions must stay valid, which they do when they are static.

### Making values

| Function | Since | |
|---|---|---|
| `make_int(intptr_t n)` | 1 | An Integer. Raises `OverflowError` outside Clay's 62-bit range. |
| `make_float(float f)` | 1 | A Float. Clay's floats are 32-bit. |
| `make_string(const char *s)` | 1 | A String with a copy of `s`, up to its NUL. |
| `make_bytes(const void *data, size_t len)` | 4 | A String with a copy of exactly `len` bytes, NULs included. |
| `make_bool(int truthy)` | 1 | `true` or `false`. |
| `make_nil(void)` | 1 | nil: the same as `CLAY_NIL`. |

### Reading values

| Function | Since | |
|---|---|---|
| `int is_int(v)` | 1 | 1 for an Integer. |
| `int is_float(v)` | 1 | 1 for a Float. |
| `int is_string(v)` | 1 | 1 for a String. |
| `int is_nil(v)` | 1 | 1 for nil. |
| `int is_array(v)` | 2 | 1 for an Array. |
| `int is_dict(v)` | 5 | 1 for a Dict. |
| `int is_error(v)` | 6 | 1 for a raised error; 0 for break, continue and non-local return, and for a value that isn't an object. |
| `intptr_t to_int(v)` | 1 | An Integer's value. A Float is truncated toward zero (`OverflowError` if it's out of range). Anything else gives 0. |
| `float to_float(v)` | 1 | A Float's value, or an Integer's. Anything else gives 0. |
| `const char *to_string(v)` | 1 | A String's text, as a C string. Anything else gives `""`. The text stops at the first NUL byte. |
| `const char *to_bytes(v, size_t *len)` | 4 | A String's bytes, with their count in `*len` (NUL-terminated as well). `NULL` and 0 for anything else. |

`to_int`, `to_float` and `to_string` don't convert: `to_string` on the number 5 is `""`, not
`"5"`. Check the type first when it matters, and raise if it's wrong. For a value's text, send
it `toString`. The pointers from `to_string` and `to_bytes` are valid while the String is alive;
an argument is, until the method returns.

### Objects and messages

| Function | Since | |
|---|---|---|
| `ClayPluginObject clone(proto)` | 1 | A new object cloned from `proto`, a new reference. For a type with a `data_size`, the clone gets its buffer. |
| `ClayPluginValue send(recv, const char *sel, int argc, ClayPluginValue *argv)` | 1 | Sends a message, as Clay code does, and returns the result as a new reference. An error in it unwinds past the caller. |
| `int try_send(recv, sel, argc, argv, ClayPluginValue *out)` | 6 | `send` that catches errors: see [Errors](#errors). |
| `void raise(const char *msg, const char *type)` | 1 | Raises an error. Does not return. |
| `void raise_value(v)` | 6 | Raises `v` (an error `try_send` caught, say), taking its reference. Does not return. |
| `void set_slot(obj, const char *name, v)` | 1 | Sets a slot on `obj` itself; the slot takes its own reference. |
| `ClayPluginValue get_slot(obj, const char *name)` | 1 | A slot's value, looked up along the prototype chain: borrowed, nil if there is none. |
| `void set_data(obj, void *data)` / `void *get_data(obj)` | 1 | An object's native data: see [Native data](#native-data-wrapping-a-c-struct). |

`sel` is the message name as Clay writes it: `send(x, "size", 0, NULL)` is `x size`,
`send(x, "add", 1, &y)` is `x add(y)`, and operators are their symbols (`"+"`, `"=="`).
`argv` may be NULL when `argc` is 0.

### Reference counts

| Function | Since | |
|---|---|---|
| `void retain(v)` | 2 | Keeps `v` alive past the call it was handed to. |
| `void release(v)` | 2 | Gives up a reference. Does nothing on numbers, nil and booleans. |

### Arrays

| Function | Since | |
|---|---|---|
| `ClayPluginValue array_new(void)` | 2 | An empty Array, a new reference. |
| `size_t array_size(arr)` | 2 | Its length; 0 for a value that isn't an Array. |
| `ClayPluginValue array_get(arr, size_t i)` | 2 | Element `i`, borrowed; nil past the end or on a non-Array. |
| `void array_push(arr, v)` | 2 | Appends `v`, retaining it. Does nothing on a non-Array. |
| `size_t to_floats(arr, float *out, size_t max)` | 2 | Copies the numbers in `arr`, nested arrays flattened in order (rows of a matrix), writing at most `max`. Returns how many numbers there are, so call it with `max` 0 to size the buffer. `TypeError` on anything else. |
| `ClayPluginValue from_floats(const float *data, size_t n, size_t cols)` | 2 | An Array of `n` numbers, or with `cols` > 0, of `n / cols` rows of `cols` numbers. A new reference. |

`to_floats` and `from_floats` move numeric data in one call, which is much faster than an
element at a time. Size the buffer first so an error can't leak it:

```c
size_t n = api->to_floats(argv[0], NULL, 0);     /* raises here, before any malloc */
float *buf = malloc(n ? n * sizeof(float) : 1);
api->to_floats(argv[0], buf, n);                 /* can't raise now: same array */
```

### Dicts

Dict keys passed to and from a plugin are strings, and a Dict keeps its keys in the order they
were added.

| Function | Since | |
|---|---|---|
| `ClayPluginValue dict_new(void)` | 5 | An empty Dict, a new reference. |
| `void dict_set(d, const char *key, v)` | 5 | Stores `v` under `key`, retaining `v` and copying `key`. |
| `ClayPluginValue dict_get(d, const char *key)` | 5 | The value under `key`, borrowed; nil if it's missing. |
| `int dict_has(d, const char *key)` | 5 | 1 if `key` is present (it may hold nil). |
| `size_t dict_size(d)` | 5 | The number of entries. |
| `int dict_next(d, size_t *pos, const char **key, ClayPluginValue *v)` | 5 | Walks the entries: start with `*pos = 0`. Each call sets `*key` and `*v` (borrowed) and returns 1, or returns 0 after the last one. |

On a value that isn't a Dict, `dict_set` does nothing, `dict_get` returns nil, and the others
return 0. Don't add or remove keys while walking.

### Output

| Function | Since | |
|---|---|---|
| `void write_output(const char *text)` | 3 | Writes `text` where Clay's own output goes: stdout, or the Debug Console under `clay --dap`. |

## Native data: wrapping a C struct

Each object can carry a pointer to C data, its *native data*. It is how a plugin gives a Clay
object a C struct (a handle, a buffer, a matrix), and `free_fn` is how that data is cleaned up
when Clay frees the object.

### With `data_size` (version 6)

Give the type a `data_size`, and every object `api->clone` makes gets its own zeroed buffer of
that many bytes. Clay owns the buffer: it calls `free_fn` (if there is one) when the object is
freed, then frees the buffer itself. So `free_fn` releases only what the struct points to, and
**must not free the struct**.

```c
typedef struct { char *label; long hits; } Box;

static void box_free(void *data) {
    Box *b = data;
    free(b->label);                  /* what the struct owns; not b itself */
}

static ClayPluginValue box_new(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    const char *s = argc > 0 && api->is_string(argv[0]) ? api->to_string(argv[0]) : "";
    ClayPluginObject obj = api->clone(self);
    Box *b = api->get_data(obj);     /* zeroed */
    b->label = strdup(s);
    return (ClayPluginValue)obj;
}

static ClayPluginValue box_hit(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    Box *b = api->get_data(self);
    if (!b) { api->raise("use Box new", "RuntimeError"); return CLAY_NIL; }
    b->hits++;
    return (ClayPluginValue)self;
}

/* in the init function */
ClayPluginType def = { "Box", methods, box_free, sizeof(Box) };
api->register_type(&def);
```

Because the buffer is freed with the object, a method that raises after `clone` can't leak it.

### With your own allocation

Allocate the struct yourself, attach it with `set_data`, and free it in `free_fn`. This is how
plugins written for Clay before version 6 work, and how to attach data that isn't a fixed-size
struct:

```c
static void acc_free(void *data) { free(data); }

static ClayPluginValue acc_new(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    ClayPluginObject obj = api->clone(self);
    api->set_data(obj, calloc(1, sizeof(Acc)));
    return (ClayPluginValue)obj;
}

ClayPluginType def = { "Acc", methods, acc_free, 0 };
```

If the type also has a `data_size`, `set_data` frees the buffer Clay made before attaching your
data, and from then on `free_fn` owns what you attached.

### Which objects have native data

- `get_data` returns NULL for an object without any: the prototype itself, an object made by
  Clay's own `clone` message (`Box clone`, or a wrapper like `MyBox = Box clone { ... }`), and
  any object that isn't a plugin object. **Every method that uses native data checks for NULL**,
  because Clay code can send it to any of these.
- A Clay wrapper cloned from the prototype keeps its methods and its `free_fn` and `data_size`,
  so `api->clone(self)` in a constructor called on the wrapper (`MyBox new`) makes an object of
  the wrapper with its own buffer, and `type` on it says `MyBox`.
- `set_data` works on any object, and replaces what was there without freeing it (except Clay's
  own `data_size` buffer). Free the old data first if you replace it.
- Native data isn't seen by Clay's cycle collector. If it holds a Clay value (`retain`ed), the
  object keeps that value alive; release it in `free_fn`.

## Loading

- `System plugin(name)` loads `plugins/<name>.so`, `.dylib` or `.dll`, looking in the current
  directory, then next to the running script, then next to the `clay` executable. A path also
  works. It returns the last prototype the plugin registered (or `true` if it registered none),
  and raises `PluginError` if no library is found.
- Every library in the `plugins/` folder beside the `clay` executable is loaded at startup.
- A native brick's plugin is built and loaded when the brick is imported, and linked into the
  executable by `clay --compile`.
- Loading a plugin a second time does nothing: its init function runs once.
- `System pluginGlobals` lists the names of the prototypes plugins have registered.
- A program can load at most 32 plugin libraries, and plugins can register at most 64 globals.
  A prototype registered past that limit still works through the value `register_type` returns,
  but has no global name.
- Plugins stay loaded until the program ends.

### Threads

Plugin methods run on whichever thread runs the Clay code that calls them. In a build with
`-DCLAY_THREAD_SAFE=ON`, `Thread` and `parallelMap` can call one plugin from several threads at
once, so guard any state the plugin shares between calls (statics, globals). A plugin must not
call the API from a thread it started itself.

## Versions

The table only grows: new functions are added at the end, and `api_version` says how far it
goes. A plugin built for an older version keeps working in a newer Clay.

| Version | Clay | Added |
|---|---|---|
| 1 | | `register_type`, `register_methods`, `make_*`, `to_*`, `is_int`/`is_float`/`is_string`/`is_nil`, `clone`, `send`, `set_data`/`get_data`, `raise`, `set_slot`/`get_slot` |
| 2 | | `api_version`, `retain`/`release`, arrays, `to_floats`/`from_floats` |
| 3 | | `write_output` |
| 4 | | `make_bytes`, `to_bytes` |
| 5 | 0.3.5 | dicts |
| 6 | 0.3.13 | `try_send`, `raise_value`, `is_error`; `ClayPluginType.data_size` allocates native data; `to_int` on a Float truncates it (before, it gave a meaningless number) |

To use a newer function and still load into an older Clay, check first:

```c
if (api->api_version >= 6) {
    /* try_send available */
}
```

`data_size` has no field of its own to check. In a Clay before version 6 it did nothing, so a
plugin that relies on it should refuse to load there, or use its own allocation.

---

[← Native compilation, FFI and plugins](native-and-plugins.md) · [Guide index](README.md)
