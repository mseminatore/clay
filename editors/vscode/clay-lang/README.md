# Clay Language for VS Code

Syntax highlighting, code snippets, debugging, and language configuration for the [Clay](../../../README.md) programming language.

## Features

- **Syntax highlighting** for `.clay` files
  - Keywords: `self`, `super`, `import`, `try`, `catch`, `finally`, `raise`
  - Constants: `true`, `false`, `nil`
  - Built-in types: `Object`, `Number`, `Float`, `String`, `Array`, `Block`, `Boolean`, `Error`, `Dict`, `Nil`, `Math`, `Console`, `File`, `System`, `Date`, `Http`, `Regex`, `Fiber`, `Db`
  - String interpolation: `"hello ${name}"`
  - String escapes: `\n`, `\t`, `\r`, `\\`, `\"`, `\0`, `\xHH`
  - Numbers: integers (`42`), floats (`3.14`), scientific notation (`1e5`)
  - Operators: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`, `!`
  - Compound assignment: `+=`, `-=`, `*=`, `/=`, `%=`
  - Comments: `//` line and `/* */` block
  - Block parameters: `{ |x, y| ... }`
- **Language configuration** — bracket matching, auto-closing, comment toggling, indentation
- **Code snippets** — `clone`, `method`, `block`, `try`, `trycatchfinally`, `while`, `if`, `ifelse`, `import`, `raise`, `println`, `each`, `map`, `dict`, `super`, `getslot`
- **DAP debugging** — set breakpoints, step through code, inspect variables via the Clay DAP server (`clay --dap`)

## Installation

### Option 1: Symlink (recommended for development)

**Windows (PowerShell as Administrator):**
```powershell
$ext = "$env:USERPROFILE\.vscode\extensions\clay-lang"
$src = (Resolve-Path "editors\vscode\clay-lang").Path
New-Item -ItemType Junction -Path $ext -Target $src
```

**macOS/Linux:**
```bash
ln -s "$(pwd)/editors/vscode/clay-lang" ~/.vscode/extensions/clay-lang
```

### Option 2: Copy

Copy the `editors/vscode/clay-lang` folder into `~/.vscode/extensions/`.

### After installation

Restart VS Code (or run **Developer: Reload Window**). Open any `.clay` file to see syntax highlighting.

## Debugging

The extension includes a Debug Adapter Protocol (DAP) client for the Clay VM debugger.

1. Open a `.clay` file
2. Set breakpoints by clicking the gutter
3. Press **F5** or use **Run > Start Debugging**
4. Select the **Clay Debug** configuration

The debugger supports stepping (F10/F11), continue (F5), breakpoints, variable inspection, and stack traces.

### launch.json example

```json
{
  "type": "clay",
  "request": "launch",
  "name": "Debug Clay Program",
  "program": "${file}",
  "clayPath": "clay"
}
```

## Snippets

| Prefix             | Description                              |
|--------------------|------------------------------------------|
| `clone`            | Clone an object with slots & methods     |
| `method`           | Method definition                        |
| `block`            | Block with parameters                    |
| `try`              | Try/catch error handling                 |
| `trycatchfinally`  | Try/catch/finally error handling         |
| `while`            | While loop with whileTrue                |
| `if`               | ifTrue conditional                       |
| `ifelse`           | ifTrue/ifFalse chain                     |
| `import`           | Import a Clay file                       |
| `raise`            | Raise an error                           |
| `println`          | Print with newline                       |
| `each`             | Iterate over a collection                |
| `map`              | Map over a collection                    |
| `dict`             | Create a new Dict                        |
| `super`            | Call a method on the parent prototype    |
| `getslot`          | Retrieve a slot without invoking blocks  |
