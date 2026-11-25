# cluau for Visual Studio Code

[cluau](https://github.com/Aosiee/cluau) is C-syntax-flavoured Luau: braces,
C operators, `let` / `const`, classes, `try` / `catch` and enums, translated
to standard Luau with every line on its line. This extension gives `.cluau`
files:

- highlighting;
- the checker's errors and warnings as you type (`cluau check`: Luau's type
  checker in strict mode and cluau's strict rules), for the file and the
  modules it requires;
- completion of names, members, methods and the standard modules
  (`@std/fs`, `@std/json`, ...), with their types;
- hover (a name's type and the comment above its declaration) and signature
  help in calls;
- go to definition (locals, functions, class members across modules, and
  `require` paths), references and renaming of local names, and the outline
  of classes, functions and variables;
- formatting (`cluau fmt`);
- the debugger (`cluau debug`): breakpoints with conditions, hit counts and
  log messages, pausing on errors, stepping, the paused frames' variables,
  and expressions evaluated in them; F5 debugs the current file;
- **cluau: Run File**, which runs the file in a terminal, and **cluau:
  Debug File**.

## Requirements

The `cluau` program, 0.16.0 or later, on `PATH`; the extension starts
`cluau lsp` and `cluau debug`. To install it:

```sh
curl -fsSL https://github.com/Aosiee/cluau/releases/latest/download/install.sh | sh
```

```powershell
irm https://github.com/Aosiee/cluau/releases/latest/download/install.ps1 | iex
```

## Settings

| setting | is |
|---|---|
| `cluau.path` | the `cluau` program, when it is not on `PATH` |
| `cluau.modules` | a host's modules, typed from files: `{ "@game/world": "types/world.luau" }` (as `cluau check --module`) |
| `cluau.trace.server` | logs the messages to and from the language server |

Bare `require` names are looked for as `cluau` looks for them: the script's
directory, its `cluau_modules`, then `CLUAU_PATH`.

## Debugging

F5 on a `.cluau` file debugs it without a `launch.json`. A configuration
of type `cluau` takes `program`, `args`, `cwd` (the script's directory when
not given), `stopOnEntry` and `sandbox`:

```json
{
	"type": "cluau",
	"request": "launch",
	"name": "Debug main.cluau",
	"program": "${workspaceFolder}/main.cluau",
	"args": ["--verbose"]
}
```

The script's output appears in the debug console; its standard input is
empty.
