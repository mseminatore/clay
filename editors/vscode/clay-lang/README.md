# Clay Language for VS Code

Syntax highlighting, code snippets, and language configuration for the [Clay](../../../README.md) programming language.

## Features

- **Syntax highlighting** for `.clay` files
  - Keywords: `self`, `import`, `try`, `catch`, `raise`
  - Constants: `true`, `false`, `nil`
  - Built-in types: `Object`, `Number`, `String`, `Array`, `Block`, `Boolean`, `Error`, `Nil`
  - String interpolation: `"hello ${name}"`
  - Comments: `//` line and `/* */` block
  - Operators, numbers, block parameters
- **Language configuration** — bracket matching, auto-closing, comment toggling, indentation
- **Code snippets** — `clone`, `method`, `block`, `try`, `while`, `if`, `ifelse`, `import`, `raise`, `println`

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

## Snippets

| Prefix    | Description                          |
|-----------|--------------------------------------|
| `clone`   | Clone an object with slots & methods |
| `method`  | Method definition                    |
| `block`   | Block with parameters                |
| `try`     | Try/catch error handling             |
| `while`   | While loop with whileTrue            |
| `if`      | ifTrue conditional                   |
| `ifelse`  | ifTrue/ifFalse chain                 |
| `import`  | Import a Clay file                   |
| `raise`   | Raise an error                       |
| `println` | Print with newline                   |
