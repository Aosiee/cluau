# Scripting with cluau

The `cluau` tool runs cluau as a scripting language, the way `python` runs
Python: no host, no build step. A script is translated in *script mode*,
compiled by Luau and run on Luau's VM, with `require` for other files and a
small standard library for files, paths, processes, standard input and
output, and JSON.

```
#!/usr/bin/env cluau
const fs = require("@std/fs")
const process = require("@std/process")

class Counter {
	Counts: { [string]: number } = {}

	function Add(word: string) {
		self.Counts[word] = (self.Counts[word] or 0) + 1
	}
}

const counter = Counter()
for (word in string.gmatch(fs.read(process.args[1]), "%a+")) {
	counter:Add(string.lower(word))
}
print(repr(counter.Counts))
```

## Running

| command | runs |
|---|---|
| `cluau script.cluau [args...]` | a script (also `cluau run script.cluau ...`) |
| `cluau -e 'print(1 + 2)' [args...]` | code from the command line |
| `cluau - [args...]` | the script on standard input |
| `cluau` | the interactive prompt (or standard input, when it is not a terminal) |
| `cluau -i` | the interactive prompt, even when standard input is not a terminal |
| `cluau check script.cluau...` | type-checks scripts without running them (see [Checking](#checking)) |

The arguments after the script are `process.args`. A script's first line
may be `#!/usr/bin/env cluau`, so a script marked executable runs by itself
once `cluau` is on `PATH` (the installers in the README put it there; so
does `cmake --install build --prefix ~/.local`).
A `.luau` file runs as Luau (it is not translated).

The exit status is 0, 1 after an error, 2 when the script cannot be read,
130 after Ctrl-C, or the code passed to `process.exit(code)`.

## Errors

An error prints its message at the script's line, then the stack, each
frame with its line of source. Every translated line stays on its line, so
the lines are the `.cluau` file's:

```
errors.cluau:3: attempt to index nil with 'field'
stack backtrace:
errors.cluau:3 function Inner
    return (t.missing :: { field: number }).field
errors.cluau:7 function Outer
    Inner({})
errors.cluau:11
    Outer()
```

A syntax error is reported before anything runs, with its line and
column, and a caret under the place:

```
syntax_error.cluau:2:12: this '{' has no closing '}'
    if (x > 0) {
               ^
```

## Checking

`cluau check script.cluau` type-checks a script, and every module it
requires, without running it. This is Luau's type checker in strict mode
plus cluau's strict rules ([syntax.md](syntax.md#strict-typing)): no `any`
without an `// allow-any: <reason>` on the line, annotated parameters and
return types, and `const` locals that are never assigned again. Positions
are the `.cluau` file's:

```
function Greet(name: string): string {
	return "hello, " .. name
}

const count = 1
print(Greet(42))
print(Greeet("x"))
count = 2
```

```
$ cluau check greet.cluau
greet.cluau:6:13: Expected this to be 'string', but got 'number'
greet.cluau:7:7: Unknown global 'Greeet'; consider assigning to it first
greet.cluau:8:1: 'count' is const and cannot be assigned
cluau check: 3 error(s), 0 warning(s)
```

The exit status is 1 when there is an error (warnings alone are 0), so it
fits a CI step or a pre-commit hook. Running a script does not check it:
like Python, `cluau script.cluau` runs whatever parses.

Two options change what is checked:

- `--loose` checks with Luau's strict mode alone, without cluau's strict
  rules: `any` needs no exemption and functions need no annotations
  beyond what Luau itself asks for. A type error is still an error. It
  suits throwaway scripts and code on its way to full typing.
- `--lint` adds Luau's lint warnings: unused locals and functions,
  shadowed names, unreachable code, duplicate keys and conditions, and
  more. They are warnings, so they do not change the exit status. Prefix a
  name with `_` to say it is unused on purpose.

- Modules resolve exactly as `require` resolves them at run time, and a
  module's own errors are reported at its file. A module that is not found
  is an error naming where it was looked for.
- The standard modules are typed (their functions as the tables below
  give them). `json.decode` returns `unknown`: cast it to the shape you
  expect (`json.decode(text) :: { name: string }`).
- Classes are typed: calling one checks the arguments against `Init`'s
  parameters, an instance has exactly the declared fields, methods and
  statics, and an operator on instances checks against the class's
  metamethod (`a + b` needs `__add`). Errors name classes, subclasses
  included (`Type 'Bird' does not have key 'Fly'`).
- A subclass of another module's class that declares no `Init` of its own
  gets a warning: its constructor's arguments cannot be checked. Declaring
  `Init` (calling `super:Init(...)`) checks them.
- A `.luau` module is checked in its own mode (nonstrict unless it starts
  with `--!strict`).
- To catch a module's load errors and keep its type, wrap the `require`
  in a function: `const ok, mod = pcall(function() { return require("./x") })`.
  The module must exist for the check. `pcall(require, "./x")` (for a
  module that may be absent) gives `any`, so its use needs an exemption.

## The interactive prompt

`cluau` with no arguments reads one input at a time. An expression prints
its value (`repr`'s form: strings quoted, tables with their contents), and
the last value is `_`. An input whose braces, brackets, comment or long
string are still open continues on the next line (`>>`); an empty line
ends it early. A pasted block arrives whole.

```
> let words = { "a", "b" }
> #words
2
> function Shout(s: string): string {
>>     return string.upper(s) .. "!"
>> }
> Shout(words[1])
"A!"
```

At the prompt a top-level `let`, `const` or `local` sets a global (each
input is its own chunk, so a local would end with its line), and a class
is a global too.

- Tab completes globals, keywords, a module's or a table's members
  (`string.up` to `string.upper(`), and an instance's methods after `:`,
  its class's and its base's included.
- Ctrl-C abandons the input being typed (a line, or a statement over
  several) and gives a new prompt; while code runs, it stops the code. On
  an empty prompt, Ctrl-C and Ctrl-D leave.
- The history is kept in `~/.cluau_history`.

A script waiting in `input()` or `io.readLine()` stops at the first
Ctrl-C too (`interrupted`, exit status 130).

## Classes in scripts

In script mode a file may declare any number of classes, anywhere at its
top level, and each is a local that the code after it uses. Calling a
class constructs an instance:

```
class Vec {
	X: number = 0
	Y: number = 0
	static Count = 0

	function Init(x: number, y: number) {
		self.X = x
		self.Y = y
		Vec.Count += 1
	}

	function __add(other: Vec): Vec {
		return Vec(self.X + other.X, self.Y + other.Y)
	}

	function __tostring(): string {
		return `Vec({self.X}, {self.Y})`
	}
}

let v = Vec(1, 2) + Vec(3, 4)
print(v, Vec.Count)   // Vec(4, 6)    3
```

- `Name(args...)` makes an instance: each field gets its default, then the
  class's `Init(args...)` runs when it declares one.
- A field's default is evaluated for each instance, so `Items: {string} = {}`
  gives every instance its own table. A field without a default starts as
  `nil`.
- Methods are called with `:` (`v:Length()`); a `static` member is the
  class's own (`Vec.Count`) and instances read it too.
- The class is its instances' metatable, so a method named `__tostring`,
  `__eq`, `__lt`, `__le`, `__add`, `__sub`, `__mul`, `__div`, `__unm`,
  `__concat`, `__len` or `__call` is that metamethod.
- `repr(instance)` shows its class and fields (`Vec {X = 4, Y = 6}`) unless
  the class has a `__tostring`.

A file whose last statement is a class returns it, so a module may be one
class (`return` is not needed), as in a host's class files.

### Subclasses

`class Name extends Base` makes a class whose instances are also `Base`'s:

```
class Animal {
	Name: string = "?"

	function Init(name: string) {
		self.Name = name
	}

	function Speak(): string {
		return "..."
	}

	function Describe(): string {
		return `{self.Name} says {self:Speak()}`
	}
}

class Bird extends Animal {
	Legs: number = 2

	function Init(name: string, legs: number) {
		super:Init(name)
		self.Legs = legs
	}

	function Speak(): string {
		return "tweet"
	}
}

print(Bird("Pip", 2):Describe())   // Pip says tweet
```

- A subclass has its base's fields (with their defaults), methods, statics
  and metamethods, and adds or replaces its own. A method it declares
  replaces the base's for its instances, including where the base's own
  methods call it (`Describe` above calls `Bird`'s `Speak`).
- `super:Method(args)` calls the base's method on `self`;
  `super.Name` is the base's member. Without its own `Init`, a subclass
  is constructed with its base's.
- The base is any class value: one declared earlier in the file, or a
  module's (`class Big extends shapes.Square`, after
  `const shapes = require("./shapes")`).
- `cluau check` types an instance as its base's and its own members, so a
  `Bird` goes wherever an `Animal` is expected. The constructor's
  arguments are checked against `Init` (or the base's, when the base is in
  the same file; a subclass of another module's class that declares no
  `Init` is constructed unchecked, and `cluau check` warns about it).

## Modules

`require(name)` loads a `.cluau` or `.luau` file once and returns what it
returns (`true` when it returns nothing); later calls give the same value.

| name | is looked for |
|---|---|
| `"./name"`, `"../name"` | from the requiring file's directory |
| `"name"`, `"dir/name"` | in the main script's directory, then each `CLUAU_PATH` directory (`:`-separated; `;` on Windows) |
| `"/abs/name"` | at that path |
| `"@std/name"` | a standard module |

For each, `name`, `name.cluau`, `name.luau`, `name/init.cluau` and
`name/init.luau` are tried in that order. A module that requires itself,
directly or through others, is an error (`require cycle`).

```
// lib/greet.cluau
function Hello(name: string): string {
	return `hello, {name}`
}
return { Hello = Hello }

// main.cluau
const greet = require("./lib/greet")
print(greet.Hello("cluau"))
```

## The standard library

Luau's own libraries are all there: `string` (with `string.split`),
`table`, `math`, `os` (`os.time`, `os.clock`, `os.date`), `coroutine`,
`utf8`, `bit32`, `buffer`, `vector`, `debug`. On top of them, the globals
`require`, `repr(value): string` and `input(prompt?): string?` (a line from
standard input, `nil` at its end), and these modules:

### `@std/fs`

| function | does |
|---|---|
| `read(path): string` | the file's contents |
| `write(path, text)` | replaces the file's contents (creating it) |
| `append(path, text)` | adds to the end of the file |
| `exists(path)`, `isFile(path)`, `isDir(path)`: `boolean` | what is at `path` |
| `list(dir): {string}` | the names in a directory, sorted |
| `walk(dir): {string}` | every file under a directory (paths), sorted |
| `mkdir(path)` | creates a directory and its parents |
| `remove(path, all?)` | removes a file or an empty directory; `all` removes a directory's contents too |
| `rename(from, to)`, `copy(from, to)` | moves, or copies (directories recursively) |
| `size(path): number`, `modified(path): number` | the size in bytes; the last change, in seconds since 1970 |
| `tempDir(): string` | the system's temporary directory |

A failure is an error naming the call, the path and why
(`fs.read: cannot read 'x.txt': no such file or directory`); `pcall` it to
handle it.

### `@std/path`

`join(...)`, `dirname(p)`, `basename(p)`, `stem(p)` (the name without its
extension), `extension(p)` (with its dot: `".txt"`), `normalize(p)`
(`a/./b/../c` is `a/c`), `absolute(p)`, `relative(p, base?)` (from the
current directory by default), `isAbsolute(p)`. Paths come back with `/` on
every platform, Windows included (which accepts them).

### `@std/process`

| member | is |
|---|---|
| `args: {string}` | the script's arguments |
| `script: string` | the script's path (`"-e"`, `"stdin"`; `""` at the prompt) |
| `platform: string` | `"linux"`, `"macos"`, `"windows"` or `"other"` |
| `executable: string` | the path of the running `cluau` (to start another script with the same one) |
| `env(name): string?`, `setEnv(name, value?)` | an environment variable (`nil` unsets it) |
| `cwd(): string`, `chdir(dir)` | the working directory |
| `run(command, options?): { code: number, output: string, error: string }` | runs a command to its end, capturing its standard output |
| `exec(command, options?): number` | runs a command with the script's own output; its exit code |
| `sleep(seconds)` | waits |
| `exit(code?)` | ends the script (0 by default) |

A command is a list or a string:

- `{ "git", "commit", "-m", message }` starts the program with exactly these
  arguments. There is no shell in between, so spaces, quotes and `$` in an
  argument reach the program as they are: the safe form for anything built
  from input.
- `"ls *.txt | wc -l"` is a command line for the system shell (`/bin/sh`;
  `cmd.exe` on Windows), for pipes, globs and redirections.

The options (a table, all optional):

| option | is |
|---|---|
| `input: string` | the command's standard input (by default it reads the script's) |
| `cwd: string` | the directory it runs in |
| `env: { [string]: string }` | variables set on top of the script's environment |
| `stderr` (run only) | `"inherit"` (the default: it goes where the script's goes), `"capture"` (into `error`) or `"merge"` (into `output`, in order) |

```
const process = require("@std/process")

const status = process.run({ "git", "status", "--short" }, { cwd = "repo" })
if (status.code != 0) {
	print("git failed")
}
const sorted = process.run({ "sort" }, { input = "b\na\n" }).output
```

A program that cannot be started (not found, not executable, a missing
`cwd`) is an error; a program that starts and fails is its exit code.

### `@std/io`

`write(...)` writes its arguments to standard output with no separator and
no newline; `writeError(...)` writes to standard error; `readLine():
string?` reads a line (without its newline; `nil` at the end of input);
`readAll(): string` reads the rest; `flush()`.

### `@std/json`

`encode(value, pretty?): string` and `decode(text): any`. `pretty` is
`true` (two spaces) or an indent string. Objects' keys are written sorted,
so the output is the same every run; a table whose keys are `1..n` is an
array, and an empty table is `[]`. `null` decodes to `nil` (a key whose
value is `null` is absent). A value with no JSON form (a function, a
table that contains itself, NaN) is an error.

## Embedding the runtime

The runtime is a library (`cluau_runtime`, `include/cluau/runtime.h`):
`cluau::runtime::Runtime` runs a file (`RunFile`), source text
(`RunSource`) or the prompt (`Repl`), and `State()` is its `lua_State` for a
host to add its own globals; `cluau::runtime::Check` is `cluau check`. It
builds against Luau's VM, compiler and analysis: the checkout
`CLUAU_LUAU_DIR` names, else the Luau release cluau pins, which CMake
fetches (`CLUAU_LUAU_VERSION` picks another). `CLUAU_BUILD_RUNTIME` is on for a standalone build
and off under `add_subdirectory`, so a host that embeds only the translator
needs no Luau.
