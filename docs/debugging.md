# Debugging and editor support

Clay has three debugging tools: bytecode tracing, an interactive stepper, and a
Debug Adapter Protocol server for full source-level debugging in VS Code.
Natively compiled programs can also be debugged with gdb, lldb or Visual Studio,
which step through Clay source via `#line` directives (see
[native compilation](native-and-plugins.md)).

## Tracing and stepping

**Bytecode tracing** (`--trace`) shows each VM opcode as it executes:

```
$ clay --trace
>>> 2 + 3
0000  PUSH_CONST      0
0003  PUSH_CONST      1
0006  PUSH_CONST      2
0009  CALL_NATIVE     768 3
0013  HALT
=> 5
```

**Interactive stepping** (`--step`) pauses at each source line:

```
$ clay --step
>>> x = 10
  1 | x = 10
[step] s
Stack (0):
[step] n
>>>
```

Step commands: `n` = next line, `c` = continue, `s` = show stack, `q` = quit.

**VS Code Debugging** (`--dap`) provides a full Debug Adapter Protocol server for VS Code:

- Set breakpoints by clicking in the gutter
- **Conditional breakpoints** — right-click a breakpoint, set a condition expression (e.g., `x > 10` or `name`)
- **Hit count breakpoints** — break only on the Nth hit (right-click → Edit Hit Count)
- Step through code line by line (Next, Step In, Step Out)
- **Pause** running programs with the pause button
- **Watch expressions** — evaluate variables and dot-notation (e.g., `obj.slot.nested`) in the debug console and on hover
- Inspect global variables in the Variables panel (expand objects and arrays)
- View the call stack
- Pause on exceptions

To use: install the Clay extension from `editors/vscode/clay-lang/`, add a launch configuration:

```json
{
  "type": "clay",
  "request": "launch",
  "name": "Debug Clay",
  "program": "${file}",
  "clayPath": "path/to/clay"
}
```

Then press F5 to start debugging.

## VS Code extension

**Syntax highlighting:** A TextMate grammar extension is included at `editors/vscode/clay-lang/`. To install:

```sh
# macOS / Linux — symlink into your extensions folder
ln -s "$(pwd)/editors/vscode/clay-lang" ~/.vscode/extensions/clay-lang
```

```powershell
# Windows (PowerShell as Admin) — create a junction
$ext = "$env:USERPROFILE\.vscode\extensions\clay-lang"
New-Item -ItemType Junction -Path $ext -Target (Resolve-Path "editors\vscode\clay-lang").Path
```

Then restart VS Code. See [`editors/vscode/clay-lang/README.md`](../editors/vscode/clay-lang/README.md) for details.

**Build, run & debug:** The `.vscode/` folder provides ready-to-use integration (requires CMake Tools and C/C++ extensions):

| Feature | How |
|---------|-----|
| Build all | `Ctrl+Shift+B` (default build task) |
| Run tests | `Ctrl+Shift+P` → "Run Test Task" |
| **Run current .clay file** | Launch: "Run: Current .clay File" |
| Debug test runner | Launch: "Debug: Test Runner" |
| Debug VM REPL | Launch: "Debug: VM REPL" |
| Compile to native + debug | Launch: "Debug: Compiled .clay Program" |
| Compile to C only | Task: "Compile .clay to C Only" |
| **Debug Clay program** | Launch: "Debug Clay Program" (uses DAP) |

The Clay extension includes a **Debug Adapter** that supports source-level debugging of `.clay` files. Set breakpoints, step through code, and inspect variables — all within VS Code. The adapter communicates with `clay --dap` over the Debug Adapter Protocol.

In compiled mode, `#line` directives map generated C back to `.clay` source lines, so the Visual Studio debugger steps through Clay code directly.

---

[← Guide index](README.md)
