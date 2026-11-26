# Scripting with cluau

The `cluau` tool runs cluau as a scripting language, the way `python` runs
Python: no host, no build step. A script is translated in *script mode*,
compiled by Luau and run on Luau's VM, with `require` for other files and a
standard library for files, paths, processes, standard input and output,
JSON, dates and times, HTTP, command-line arguments and CSV. The same tool
type-checks scripts (`cluau check`), runs their tests (`cluau test`) and
formats them (`cluau fmt`).

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
| `cluau test [-k text] [path...]` | runs the test cases in `*_test.cluau` files (see [Testing](#testing)) |
| `cluau fmt [--check] [path...]` | formats `.cluau` files (see [Formatting](#formatting)) |
| `cluau profile script.cluau [args...]` | runs a script and reports where its time went (see [Profiling](#profiling)) |
| `cluau lsp` | the language server, for editors (see [Editors](#editors)) |
| `cluau debug` | the debugger, for editors (see [Debugging](#debugging)) |
| `cluau --sandbox script.cluau` | a script without file, network or process access (see [The sandbox](#the-sandbox)) |

The arguments after the script are `process.args`. A script's first line
may be `#!/usr/bin/env cluau`, so a script marked executable runs by itself
once `cluau` is on `PATH` (the installers in the README put it there; so
does `cmake --install build --prefix ~/.local`).
A `.luau` file runs as Luau (it is not translated).

The exit status is 0, 1 after an error, 2 when the script cannot be read,
130 after Ctrl-C, or the code passed to `process.exit(code)`.

On x64 and arm64, scripts and the modules they require run as native code
(Luau's code generator): arithmetic, calls and loops run about 1.7 times
as fast as interpreted (code that mostly builds tables gains less), and
errors, tracebacks and Ctrl-C behave the same.
Generating it takes a few milliseconds for a long script; `--no-native`
before the script (`cluau --no-native script.cluau`) interprets it instead.

## The sandbox

`cluau --sandbox script.cluau` runs a script that cannot touch files, the
network or other programs: for a script from somewhere else, or one that
only computes.

- `@std/fs`, `@std/http`, `@std/net`, `@std/sqlite`, `@std/zip` and
  `@std/tar` are not available (`require` raises an error saying so), nor
  what is built on them (`@std/server`, `@std/websocket`).
- `@std/process` keeps `args`, `platform`, `script`, `exit` and `sleep`;
  environment variables, the working directory and running commands are
  left out.
- `require` loads `.cluau` and `.luau` modules only from under the script's
  directory, its `cluau_modules` and the search path (`CLUAU_PATH`).
- `@std/io` (standard input and output), `@std/json`, `@std/time`,
  `@std/path` (text only), `@std/regex`, `@std/task`, `@std/crypto`,
  `@std/encoding`, `@std/compress`, `@std/term`, `@std/url`, `@std/args`,
  `@std/csv` and `@std/test` work as usual, as do Luau's own libraries, which have no file or process access.

`--sandbox` goes before the command, so `cluau --sandbox -e '...'`,
`cluau --sandbox test` and the prompt (`cluau --sandbox`) are sandboxed
too. A host embedding the runtime sets `Options::Sandbox`.

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

## Testing

A test file is a script named `*_test.cluau` whose cases use `@std/test`:

```
const test = require("@std/test")
const shapes = require("./shapes")

test.case("a square's area", function() {
	test.equal(shapes.Square(3).Area, 9)
})

test.case("sides are positive", function() {
	test.raises(function() {
		shapes.Square(-1)
	}, "positive")
})
```

`cluau test` runs every `*_test.cluau` under the current directory (or
the files and directories named), each in a fresh runtime, and reports
each case as it runs:

```
$ cluau test
tests/shapes_test.cluau
  ok    a square's area
  FAIL  sides are positive
        tests/shapes_test.cluau:9: expected an error containing 'positive', but none was raised
1 passed, 1 failed (1 file(s))
```

- A case runs when it is declared. A failed assertion or an error ends
  that case, and the file goes on with the next. An error outside any case
  stops the file, which is reported as not finished.
- `-k text` runs the cases whose names contain `text` (in any case).
- The exit status is 1 when a case failed, a file did not finish, or
  nothing ran, so `cluau test` fits a CI step.
- Hidden directories (`.git`, ...) are skipped.

`--coverage` also counts the lines the tests ran, in every file they
loaded (not the test files themselves, nor packages in `cluau_modules`),
and reports the lines that never ran:

```
$ cluau test --coverage
...
coverage (lines run):
  shapes.cluau     12/13     92%   not run: 13
  total            12/13     92%
```

A line counts when it holds a statement. `--lcov <file>` writes the same
counts as LCOV, which editors' coverage extensions and CI services read.
The tests run interpreted under `--coverage`.

## Profiling

`cluau profile script.cluau [args...]` runs the script and then reports,
on stderr, where its time went: each function's own time (its code and
the C functions it calls) and its total time (with what it calls), and
the lines that took the most:

```
$ cluau profile render.cluau
...
profile: 1.204 s, 1187 ms sampled

   self   total  function
   41.2%   58.0%  Shade (render.cluau:31)
   16.8%   16.8%  Dot (vec.cluau:4)
   ...

   self  line
   22.9%  render.cluau:36
   ...
```

It samples the stack about once a millisecond, at Luau's safepoints
(calls and loops); time inside a long C call (a sleep, a read, a download)
goes to the line that made the call, and so does time in the standard
modules written in cluau (a `task.wait`). A module's top level is
`(top level)`, as the script's is. `--top n` shows `n` functions (20 by
default). `--folded file` writes the samples as folded stacks
(`outer;inner count` per line), which flame graph tools (`flamegraph.pl`,
speedscope, Inferno) draw. Native code is profiled as it runs; with
`--no-native`, a loop's time is on its `for` or `while` line rather than
its body.

## Formatting

`cluau fmt` makes a file's layout regular without moving any code to
another line, so error positions and line-based tools still match:

- Each line is indented by the brackets open at its start: one level for a
  line that opens any, a closing line back at its opener's level, and one
  more for a statement continued from the line before (`a +` then `b`, or
  a line starting with `.`, `:` or a binary operator).
- Trailing whitespace goes, and the file ends with one line break.
- Text inside strings, long strings and comments that span lines is left
  exactly as written.

`cluau fmt` formats every `.cluau` under the current directory (or the
files and directories named) in place; `--check` changes nothing and exits
with status 1 when a file would change, for CI; `--indent "    "` indents
with spaces instead of a tab; `cluau fmt -` formats standard input to
standard output. A file that does not lex (an unterminated string) is
reported and left alone.

## Editors

`cluau lsp` is a language server: an editor starts it and gets, for each
open `.cluau` file, what `cluau check` reports for it as you type (checked
with the modules it requires as the editor has them, or as they are on
disk; a required module's own problems show once it is open), completion with the types of names, members
and the standard modules, hover (a name's type and the comment above its
declaration), signature help, go to definition (locals, functions, class
members in other modules, `require` paths), references and renaming
(a local's in its file; a class member's or a module's exported name's in
every file of the workspace that uses it), the outline of classes,
functions and variables, the workspace's symbols, the types of names
declared without one shown beside them (inlay hints), highlighting by
what each name is (a class, a method, a parameter, the standard library's;
semantic tokens), the call hierarchy (what calls a function, and what it
calls), quick fixes (a misspelt name's likely names, the `require` of a
standard module used without one, a line's `allow-any` exemption), and
formatting (`cluau fmt`). It finds modules as `cluau` does, and takes the
same `--module <name>=<file>` options as `cluau check` for a host's
modules.

The VS Code extension (in `editors/vscode`; on the VS Code Marketplace and
Open VSX as *cluau*, and as a `.vsix` with each release) starts it, and
adds highlighting, the debugger (see [Debugging](#debugging)) and *Run
File* and *Debug File* commands. It needs `cluau` on `PATH` (or the
`cluau.path` setting).

Another editor runs `cluau lsp` on standard input and output for the
`cluau` file type. In Neovim (0.11 or later):

```lua
vim.filetype.add({ extension = { cluau = "cluau" } })
vim.lsp.config("cluau", { cmd = { "cluau", "lsp" }, filetypes = { "cluau" }, root_markers = { "cluau.toml", ".git" } })
vim.lsp.enable("cluau")
```

In Helix (`languages.toml`):

```toml
[language-server.cluau]
command = "cluau"
args = ["lsp"]

[[language]]
name = "cluau"
scope = "source.cluau"
file-types = ["cluau"]
comment-token = "//"
language-servers = ["cluau"]
```

## Debugging

`cluau debug` is a debugger for editors: it speaks the Debug Adapter
Protocol on standard input and output, and the editor's launch
configuration names the script. In VS Code, with the extension, F5 (or
*cluau: Debug File*) debugs the current file; a `launch.json`
configuration takes:

| attribute | is |
|---|---|
| `program` | the script (required) |
| `args` | its arguments (`process.args`) |
| `cwd` | the directory it runs in (the script's own when not given) |
| `stopOnEntry` | pause at its first line |
| `sandbox` | run it in [the sandbox](#the-sandbox) |
| `test` | run its `@std/test` cases, as `cluau test` does (the default for a `*_test.cluau` file) |
| `filter` | with `test`, only the cases whose names hold this text (`cluau test -k`) |

What it does:

- Breakpoints, in the script and the modules it requires. A condition is
  a cluau expression (`count > 10 && name != "x"`); a hit count pauses from
  that hit on (`5`), or at `== 5`, `> 5`, `< 5`, `<= 5`, or every fifth hit
  (`% 5`); a log message prints instead of pausing, with each
  `{expression}` replaced by its value. A breakpoint on a line without code
  moves to the next line that has some; one past the file's last line is
  not verified.
- Pausing on an error that ends the script (on by default), or on every
  error, including those a `try` or `coroutine.resume` catches (*All
  Errors*).
- Stepping over, into and out of calls, including into a module's top
  level from its `require`, and into and out of coroutines: a step into
  `coroutine.resume` (or a function `coroutine.wrap` made) stops in the
  coroutine, and a step that leaves it, at a yield or its end, stops where
  it was resumed. Pausing a running script, and ending it (*Stop*: it ends
  as asked, not as a failure).
- Breakpoints, pauses and *Stop* take effect while the script waits in
  `task.wait` or in a blocking call made with `@std/task` loaded; a pause
  then stops at the next line that runs.
- The paused stack: each frame's locals and upvalues, with tables and class
  instances opened field by field (an instance shows its class), and
  expressions (the watch list, hovers, the debug console) evaluated in the
  selected frame. They read the script's variables; an assignment there
  changes nothing in the script.
- The script's output, and that of the programs it runs, in the debug
  console. Its standard input is empty.

Under the debugger the script runs interpreted (as `--no-native`), at the
same speed. A test file's cases run as `cluau test` runs them, with the
summary in the console; a failing assertion is an error its case catches,
so *All Errors* pauses at it.

In Neovim, with nvim-dap:

```lua
local dap = require("dap")
dap.adapters.cluau = { type = "executable", command = "cluau", args = { "debug" } }
dap.configurations.cluau = {
	{ type = "cluau", request = "launch", name = "Debug the current file", program = "${file}", cwd = "${fileDirname}" },
}
```

In Helix, in the `cluau` language of `languages.toml` (see
[Editors](#editors)):

```toml
[language.debugger]
name = "cluau"
transport = "stdio"
command = "cluau"
args = ["debug"]

[[language.debugger.templates]]
name = "launch"
request = "launch"
completion = [{ name = "script", completion = "filename", default = "." }]
args = { program = "{0}" }
```

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

### Abstract classes

An abstract class is a base that is not constructed itself, with methods
its subclasses must define:

```
abstract class Shape {
	Name: string = "shape"

	abstract function Area(): number

	function Describe(): string {
		return `{self.Name} of area {self:Area()}`
	}
}

class Square extends Shape {
	Side: number = 1

	function Area(): number {
		return self.Side * self.Side
	}
}

print(Square():Describe())   // shape of area 1
```

- `abstract function Name(params): Type` declares a method by its
  signature, with no body. A class with one is `abstract class`.
- Constructing an abstract class is an error, and a type error to `cluau
  check`.
- A class that is not abstract defines every abstract method it inherits.
  When the bases are in the same file, a missing one is an error before
  the script runs; otherwise the first construction of the class names
  what it misses (`Square does not define Area (abstract in Shape)`).
- An abstract class may extend another and leave some of its abstract
  methods to its own subclasses.

## Modules

`require(name)` loads a `.cluau` or `.luau` file once and returns what it
returns (`true` when it returns nothing); later calls give the same value.

| name | is looked for |
|---|---|
| `"./name"`, `"../name"` | from the requiring file's directory |
| `"name"`, `"dir/name"` | in the main script's directory, then in each `cluau_modules` directory from the requiring file's up (see [Packages](#packages)), then each `CLUAU_PATH` directory (`:`-separated; `;` on Windows) |
| `"/abs/name"` | at that path |
| `"@std/name"` | a standard module |

For each, `name` (when it ends in `.cluau` or `.luau`), `name.cluau`,
`name.luau`, `name/init.cluau` and `name/init.luau` are tried in that
order; a file of any other kind is not a module. A module that requires itself,
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

## Packages

A project's dependencies are other people's (or your own) cluau code in Git
repositories. `cluau.toml`, at the project's root, names them; `cluau.lock`
pins each to an exact commit; `cluau install` fetches them into
`cluau_modules/`, where `require("name")` finds them:

```
$ cluau init
made cluau.toml for my-tool
made .gitignore (leaving cluau_modules/ out of Git)
$ cluau add github.com/someone/mathlib@v1.2.0
added mathlib (https://github.com/someone/mathlib v1.2.0) to cluau.toml
  mathlib v1.2.0 (4376c77d4c8d)
1 package(s) in cluau_modules, 1 fetched
```

```toml
[package]
name = "my-tool"
version = "0.1.0"

[dependencies]
mathlib = { git = "https://github.com/someone/mathlib", tag = "v1.2.0" }
utils = { git = "git@example.com:team/utils.git", branch = "main" }
pinned = { git = "https://github.com/someone/pinned", rev = "0007d6e4fc04c4a1d5b1e0f7a1b2c3d4e5f60718" }
```

| command | does |
|---|---|
| `cluau init [name]` | makes `cluau.toml` (and a `.gitignore` leaving `cluau_modules/` out, when there is none) |
| `cluau add <repository>[@ref] [--name <name>]` | adds a dependency and installs it. `@ref` is a tag, a branch or a commit (its id, or the first 7 or more digits of it, pinned as the whole id); without one, the newest version tag (`v1.2.3`), else the default branch. The name is the repository's last part (`my.lib.git` is `my-lib`) unless `--name` gives one. When the install fails, `cluau.toml` is left as it was |
| `cluau install` | fetches every dependency at the commit `cluau.lock` pins (resolving and pinning the new ones), and removes what is no longer wanted |
| `cluau update [name...]` | resolves the dependencies' tags and branches again (all of them, or those named) and pins the new commits |
| `cluau remove <name>` | removes a dependency |
| `cluau list` | the installed packages, their commits, and which package wanted each |

- A repository is `github.com/owner/name` (an `https://` address), any Git
  URL (`https://`, `ssh://`, `git@host:path`), or a local directory. Git
  fetches them, so `git` must be on `PATH`; private repositories use your
  Git credentials. The `@ref` of `cluau add` is the last `@` in the
  address's path, so a user in it (`ssh://git@host/a/b.git@v1`) stays.
- A package's name is letters, digits, `-` and `_`. An address or a ref
  starting with `-` is refused, in `cluau.toml` and in `cluau add`.
- A package is a repository whose `init.cluau` is what `require("name")`
  returns; `require("name/part")` loads `part.cluau` from it.
- A package's own `cluau.toml` dependencies are installed too, beside it in
  the same `cluau_modules/`. Its other keys (`authors = [...]`, a date, a
  `"""` string) are read and left alone. Two packages that want one name from different
  places or versions is an error naming both.
- Commit `cluau.toml` and `cluau.lock`; leave `cluau_modules/` out. Anyone
  with the project runs `cluau install` and gets the same commits.
- `cluau check` types packages like any module, and `cluau test` and
  `cluau fmt` leave `cluau_modules/` alone.
- For code you are working on beside a project, `CLUAU_PATH` (or a
  relative `require("../shared/x")`) is quicker than a package.

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
| `tempFile(prefix?): string` | a new, empty file of the script's own in the temporary directory |
| `glob(pattern): {string}` | the paths matching a pattern, sorted: `*`, `?` and `[a-z]` within a name, `**` for any number of directories (`fs.glob("src/**/*.cluau")`); names starting with `.` match only a pattern that does |
| `lines(path)` | the file's lines one by one, for a `for` loop (`for (line in fs.lines("log.txt")) { }`) |
| `open(path, mode?): File` | a file to read (`"r"`, the default), write from empty (`"w"`) or add to (`"a"`) |
| `readBuffer(path): buffer`, `writeBuffer(path, bytes)` | the file's bytes as a Luau `buffer`, and back |

A `File` reads and writes as it goes, bytes unchanged:

| method | does |
|---|---|
| `read(count?): string?` | `count` bytes (`nil` at the end), or without a count everything left |
| `readLine(): string?`, `lines()` | a line without its line break (`nil` at the end); every line, for a `for` loop |
| `write(...)` | writes its arguments (text or numbers) |
| `seek(offset?, from?): number` | moves to `offset` from `"set"` (the start, the default), `"current"` or `"end"`; the position |
| `flush()`, `close()` | a file closes by itself once nothing refers to it; `close` does it now |

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
| `environment(): { [string]: string }` | every environment variable |
| `cwd(): string`, `chdir(dir)` | the working directory |
| `run(command, options?): { code: number, output: string, error: string, timedOut: boolean }` | runs a command to its end, capturing its standard output |
| `exec(command, options?): (number, boolean)` | runs a command with the script's own output; its exit code, and whether it timed out |
| `spawn(command, options?): Child` | starts a command that runs alongside the script |
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
| `timeout: number` (run and exec) | seconds it may take; past them the command, and anything it started, is killed and `timedOut` is true |

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

`spawn` is for a command whose output the script reads while it runs (a
build's progress, a server's log) or one it talks to. It takes `cwd`, `env`,
`stderr = "inherit" | "merge"`, and `input = true` to write the command's
standard input. The `Child` it returns:

| method | does |
|---|---|
| `readLine(): string?`, `lines()` | the next line of its output (`nil` once it has ended); every line, for a `for` loop |
| `read(): string?` | whatever output there is next |
| `write(...)`, `closeInput()` | sends text to its standard input; ends it |
| `wait(): number` | waits for it to end (closing its input first), keeping any output left for `read`; its exit code |
| `kill()` | ends it, and anything it started |
| `id(): number` | its process id |

```
const process = require("@std/process")

const build = process.spawn({ "make", "-j8" }, { stderr = "merge" })
for (line in build:lines()) {
	if (string.find(line, "error", 1, true)) {
		print("!", line)
	}
}
print("make exited with", build:wait())
```

### `@std/io`

`write(...)` writes its arguments to standard output with no separator and
no newline; `writeError(...)` writes to standard error; `readLine():
string?` reads a line (without its newline; `nil` at the end of input);
`readAll(): string` reads the rest; `flush()`.

### `@std/json`

`encode(value, pretty?): string` and `decode(text, options?): unknown`.
`pretty` is `true` (two spaces), a number of spaces, or an indent string. Objects' keys are
written sorted, so the output is the same every run; a table whose keys are
`1..n` is an array, and an empty table is `[]`. A value with no JSON form
(a function, a table that contains itself, NaN) is an error.

- `null` decodes to `nil`, so a key whose value is `null` is absent and an
  array's `null` leaves a gap. `decode(text, { null = json.null })` decodes
  it to `json.null` instead, which encodes as `null` again.
- `json.object(t?)` and `json.array(t?)` mark a table (or a new one) to
  encode as an object or an array: `json.object()` is `{}`, not `[]`. An
  empty object decodes marked, so it encodes as `{}` again.

### `@std/time`

A time is a number: seconds since 1970-01-01 UTC, with a fraction. The
functions that read or write calendar fields take `{ utc = true }`;
without it they use the system's local time zone.

| function | is |
|---|---|
| `now(): number` | the current time |
| `monotonic(): number` | seconds on a clock that only moves forward, for measuring how long something takes |
| `parse(text): number` | an ISO 8601 date or date and time: `2026-11-24`, `2026-11-24T21:05`, `2026-11-24 21:05:09.25+01:00`, `...Z`. A date alone is UTC midnight; a time without an offset is local |
| `iso(t?, options?): string` | `2026-11-24T21:05:09.250+01:00` (`...Z` in UTC) |
| `format(t, pattern, options?): string` | `strftime`'s patterns: `%Y-%m-%d %H:%M:%S`, `%A`, `%B`, ... |
| `parts(t?, options?)` | `{ year, month, day, hour, minute, second, millisecond, weekday, yearday, offset }`: `weekday` 1 is Monday, `offset` is seconds east of UTC |
| `fromParts(parts, options?): number` | the time those fields name (all but `year` optional); a field past its range carries over, so day 32 is the next month's first |

```
const time = require("@std/time")
const start = time.monotonic()
const due = time.parse("2026-12-01") + 7 * 24 * 3600
print("due", time.format(due, "%A %d %B", { utc = true }), "took", time.monotonic() - start)
```

### `@std/http`

HTTP and HTTPS requests:

```
const http = require("@std/http")
const json = require("@std/json")

const release = http.get("https://api.github.com/repos/Aosiee/cluau/releases/latest", {
	headers = { Accept = "application/vnd.github+json" },
	timeout = 30,
})
if (release.ok) {
	print((json.decode(release.body) :: { tag_name: string }).tag_name)
}
http.post("https://example.com/hooks", nil, { json = { event = "built" } })
http.download("https://example.com/data.zip", "data.zip")
```

| function | is |
|---|---|
| `get(url, options?): Response` | a GET request |
| `post(url, body?, options?): Response` | a POST with `body` (text) or `options.json` |
| `request(options): Response` | any method: `options.url`, `options.method` (`"PUT"`, `"DELETE"`, `"HEAD"`, ...) |
| `download(url, path, options?): Response` | the body into a file, written as it arrives; the file is replaced only when the response is a 2xx |

The options: `headers` (`{ [name] = value }`), `body` (text), `json` (a
value sent as JSON, with `Content-Type: application/json`), `timeout`
(seconds for the whole request), `followRedirects` (true by default, up to
10), `caFile` (a certificate bundle to trust instead of the system's). A
body goes with its own `Content-Type` header; without one, none is sent.

A `Response` is `{ status, ok, headers, body, url }`: `ok` is a 2xx status,
`headers` has lower-case names (one value per name, repeated ones joined
with `, `), and `url` is the address the answer came from after
redirects. A 4xx or 5xx status is a response, not an error; a request that
gets no answer (an unknown host, a refused connection, a timeout, a
certificate that is not trusted) is an error naming the request and why.
Ctrl-C stops a request in progress.

HTTPS certificates are checked against the system's trusted ones: the
Windows certificate store, macOS's `/etc/ssl/cert.pem`, and on Linux the
bundle the distribution keeps (`/etc/ssl/certs/ca-certificates.crt`,
`/etc/pki/tls/certs/ca-bundle.crt`, ...). `SSL_CERT_FILE` and `SSL_CERT_DIR`
name others, as they do for other tools. Proxies come from `https_proxy`,
`http_proxy` and `no_proxy`.

### `@std/args`

A script's command line, parsed against what it declares it takes, with a
generated `--help`:

```
const args = require("@std/args")

const cli = args.parser({ name = "resize", description = "Resizes images." })
cli:flag("verbose", { short = "v", help = "say what is done" })
cli:option("width", { short = "w", help = "the new width", default = "800", number = true })
cli:option("tag", { help = "a tag to add", many = true })
cli:positional("files", { help = "the images", many = true, required = true })
const options = cli:parse()

for (_, file in options:list("files")) {
	if (options:flag("verbose")) {
		print(`resizing {file} to {options:number("width")}`)
	}
}
```

- `flag(name, { short?, help? })` is a switch: `--verbose`, `-v`.
- `option(name, { short?, help?, default?, value?, many?, number? })` takes a
  value: `--width 640`, `--width=640`, `-w 640`, `-w640`. `many` keeps every
  value given; `number` refuses one that is not a number; `value` names it
  in the help (`--width PIXELS`).
- `positional(name, { help?, many?, optional?, required? })` is an argument
  by its place: one (required unless `optional`), or with `many` every one
  left (none or more; one or more with `required`). After `--`, every
  argument is positional.
- Short flags combine (`-vq`, and `-vh` prints the help as `-h` and
  `--help` do). An argument like `-5` or `-1.5` is a negative number (a
  positional argument or a value), unless a short flag of that name is
  declared; `number` options take decimal numbers only (not `inf`, `nan`
  or `0x10`).
- `parse(list?)` reads `process.args` (or `list`). With `--help` it prints
  the help and exits; with a mistake (an unknown option, a missing value, a
  value that is not a number, a missing positional) it prints the mistake
  and the usage to standard error and exits with status 2. `tryParse(list?)`
  returns the result, or `nil` and the mistake, instead.
- The result: `flag(name): boolean`, `get(name): string?` (an option's
  value or its default, or a single positional), `number(name): number?`,
  `list(name): {string}`.
- `usage()` and `help()` are the texts it prints.

### `@std/test`

The assertions `cluau test` runs (see [Testing](#testing)). Each one fails
the case with a message at the assertion's line:

| function | checks |
|---|---|
| `case(name, fn)` | declares a case and runs it |
| `skip(name, fn)` | declares a case without running it (reported as skipped) |
| `equal(actual, expected, message?)` | equal values; tables are equal when their keys and values are |
| `notEqual(actual, unexpected, message?)` | anything but `unexpected` |
| `ok(value, message?)` | a value other than `false` or `nil` |
| `near(actual, expected, tolerance?, message?)` | numbers within `tolerance` (`1e-9` by default); NaN is near nothing |
| `raises(fn, text?): string` | `fn` raises an error (containing `text`, when given); its message |
| `fail(message)` | fails the case here |

### `@std/csv`

Comma-separated values, quoted as RFC 4180 has it (a field holding the
separator, a quote or a line break is quoted, with `""` for a quote):

| function | is |
|---|---|
| `parse(text, options?): {{string}}` | every row's fields (a byte order mark at the start is left out) |
| `records(text, options?): {{[string]: string}}` | the rows after the first, keyed by the first row's names |
| `encode(rows, options?): string` | rows (of text, numbers or booleans) as CSV, each line ending in `\n` |
| `encodeRecords(records, columns, options?): string` | a header row of `columns`, then each record's values in that order (`""` for a missing one) |

`options.separator` is another one-character separator (`";"`, `"\t"`).

### `@std/regex`

Regular expressions, as Perl and Python write them (PCRE2): alternation,
classes, quantifiers (greedy and lazy), groups (numbered and
`(?<name>...)`), lookahead and lookbehind, backreferences, `\b`, `\d`,
`\s`, `\w`, and Unicode properties (`\p{L}`). A pattern is compiled once
and matched as UTF-8 text: `\w`, `\d` and case-insensitive matching know
letters beyond ASCII. Positions are 1-based byte offsets, as `string.find`
gives them.

| function | is |
|---|---|
| `compile(pattern, flags?): Regex` | the compiled pattern; `flags` holds `i` (ignore case), `m` (`^` and `$` at each line), `s` (`.` matches a line break), `x` (spaces and `#` comments in the pattern are ignored) |
| `escape(text): string` | `text` with the pattern's special characters escaped, to match it literally |

A `Regex`'s methods (and `pattern`, its source):

| method | is |
|---|---|
| `test(text, start?): boolean` | whether it matches anywhere (from `start`) |
| `match(text, start?): Match?` | the first match (from `start`) |
| `matches(text)` | an iterator over every match: `for (m in re:matches(text)) { }` |
| `replace(text, replacement, count?): (string, number)` | the text with each match (the first `count`) replaced, and how many were. `replacement` is a string, where `$1`..`$99` are groups, `${name}` a named group, `$&` (or `$0`) the match and `$$` a dollar sign; or a function given the `Match`, returning the text |
| `split(text, limit?): {string}` | the pieces between matches (at most `limit` of them) |

A `Match` is `{ text, start, finish, groups, named }`: the matched text,
where it starts and ends, the groups by number (a group that took no part
is `nil`) and by name.

```
const regex = require("@std/regex")
const date = regex.compile("(?<year>\\d{4})-(?<month>\\d\\d)-(?<day>\\d\\d)")
const m = date:match("due 2026-11-25")
if (m != nil) {
	print(m.named.year, m.start) // 2026 5
}
print(date:replace("2026-11-25", "${day}/${month}/${year}")) // 25/11/2026 1
```

A pattern that backtracks too long on a text (nested repetition such as
`(a+)+$`) stops with an error instead of running on.

### `@std/net`

TCP connections and servers. Each wait (connecting, accepting, reading,
writing) waits on its own in a task (see [`@std/task`](#stdtask)), so one
script serves many connections, or talks to many servers, at once.

| function | is |
|---|---|
| `connect(host, port, options?): Socket` | a connection to `host` (a name or an address) on `port`; `options.timeout` limits the wait in seconds |
| `listen(host, port, options?): Server` | a server on `host` (`"127.0.0.1"` for this machine only, `"0.0.0.0"` or `"::"` for every network) and `port` (0: any free one); `options.backlog` is how many connections may wait to be accepted |

A `Server` has `host` and `port` (the port it got), `accept(options?)`
(the next connection; `options.timeout`) and `close()`.

A `Socket` has `localHost`, `localPort`, `remoteHost` and `remotePort`,
and:

| method | is |
|---|---|
| `read(count?): string?` | what has arrived, up to `count` bytes (64 KiB by default), waiting for at least one; `nil` at the end of the stream |
| `readLine(limit?): string?` | the next line, without its line break (`\r\n` or `\n`); the last line may have none; `nil` at the end. With `limit`, a line longer than that many bytes is an error (and stays unread): a server's guard against a client that never ends its line |
| `lines()` | an iterator over the lines: `for (line in socket:lines()) { }` |
| `readAll(): string` | the rest of the stream, until the other side closes it |
| `write(...)` | sends the strings, in order |
| `setTimeout(seconds?)` | how long a read or write may wait (none, or 0: no limit) |
| `close()` | ends the connection (a wait on it in another task ends with an error) |

`tls` (on a socket and a server) says whether it is encrypted.

TLS: `connect`'s `options.tls = true` encrypts the connection, checking
the server's certificate against the system's trusted certificates (or
`options.caFile`'s, a PEM file) and its name against `host` (or
`options.serverName`); `options.verify = false` skips both, for a server
of one's own with a certificate nothing trusts. `listen`'s
`options.tls = { certFile, keyFile }` (PEM files: the certificate chain and
its key) serves TLS. `accept` returns a connection as soon as it arrives,
and its handshake is made by the connection's first read or write, so a
client that is slow to begin holds up only its own task; a connection
whose handshake fails reads as one that ended (`nil`), and a write to it
is an error.

```
const net = require("@std/net")
const socket = net.connect("example.com", 443, { tls = true })
socket:write("HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
print(socket:readLine()) // HTTP/1.1 200 OK
```

```
const net = require("@std/net")
const task = require("@std/task")

const server = net.listen("127.0.0.1", 8080)
while (true) {
	const client = server:accept()
	task.spawn(function() {
		for (line in client:lines()) {
			client:write(string.upper(line), "\n")
		}
		client:close()
	})
}
```

### `@std/server`

An HTTP/1.1 server, on `@std/net` and `@std/task`: each connection is a
task of its own (several requests one after another on it), and a handler
answers each request.

| function | is |
|---|---|
| `serve(options, handler): Server` | serves `handler` and returns at once; `options.host` (`"127.0.0.1"` by default: this machine only), `options.port` (8080; 0 for any free one), `options.tls` (`{ certFile, keyFile }`), `options.maxBody` (16 MiB), `options.idle` (seconds a connection may wait between requests, 60). The `Server` has `host`, `port` and `close()` (which stops taking connections and closes those waiting for a request; a request under way is finished) |
| `router(): Router` | routes by method and path: `get`, `post`, `put`, `delete` and `route(method, pattern, handler)` (each returns the router, to chain), and `handler()` for `serve`. A pattern's parts are literal, `:name` (in `request.params.name`) or `*` (the rest, in `request.params["*"]`). The path is split at its `/`s before it is decoded, so `/users/a%2Fb` has the one part `a/b` |
| `files(directory): Handler` | serves the files under `directory` (`index.html` for a directory, the content type by extension); a path that would leave the directory, or holds a NUL, is refused, and hidden files (a part starting with `.`: `.git`, `.env`) are not found. A symbolic link inside `directory` is followed |
| `upgrade(request, fn): Answer` | answers a WebSocket request: `fn` gets the connection (a `@std/websocket` `WebSocket`), and the connection ends when `fn` returns |

A `Request` has `method`, `path` (decoded, without the query), `query`
(its names and values, decoded), `headers` (names in lowercase), `body`,
`params`, `remoteHost`, `remotePort` and `target` (as the request line gave
it). A handler answers with a table (`status`, 200 by default; `headers`;
`body`, a string or a buffer), a string (200, as text), or `nil` (404). An
error in a handler is a 500, with the error on stderr; so is a response
header whose name is not a header name or whose value has a line break.

Requests are read strictly, so that the server and anything in front of it
read the same request: a body is framed by `Content-Length` (digits only,
the same if repeated) or by `Transfer-Encoding: chunked`, not both and no
other coding (400; 501 for a coding before `chunked`), and a body that ends
early never reaches the handler (400, and the connection closes). A line of
the head may have 64 KiB, and the head 256 KiB (431; 414 for the request
line). `Expect: 100-continue` is answered with `100 Continue` before the
body is read. A failed `accept` (out of file descriptors, say) is reported
on stderr and tried again after a pause.

```
const server = require("@std/server")
const json = require("@std/json")

const routes = server.router()
routes:get("/users/:id", function(request: server.Request): server.Answer {
	return { headers = { ["content-type"] = "application/json" }, body = json.encode({ id = request.params.id }) }
})
routes:get("/static/*", server.files("public"))
server.serve({ port = 8080 }, routes:handler())
print("serving on http://127.0.0.1:8080")
```

`serve` returns at once: the server runs in tasks, so it goes on after the
script's last line until it is closed (or Ctrl-C). The rest of the script
runs alongside it, and its blocking calls wait on their own.

### `@std/websocket`

WebSocket connections: a client, and the connections `@std/server`
accepts (`server.upgrade`). Messages are text or binary; a fragmented one
arrives whole, pings are answered, and closing is the protocol's
handshake.

| function | is |
|---|---|
| `connect(url, options?): WebSocket` | a connection to a `ws://` or `wss://` address; `options.headers`, `options.protocols` (the subprotocols offered), `options.timeout`, and for `wss`, `options.caFile` and `options.verify` as `@std/net` has them |

A `WebSocket` has `send(text)`, `sendBinary(data)`, `ping(data?)`,
`receive()` (the next message, `{ kind, data }` with `kind` `"text"` or
`"binary"`, or `nil` once the connection has closed), `messages()` (an
iterator: `for (message in ws:messages()) { }`), `close(code?, reason?)`,
and `protocol`, `closeCode` and `closeReason`. A ping's data and a close's
payload are at most 125 bytes.

A frame that breaks RFC 6455 closes the connection with the code it gives
(and `receive` returns `nil`, with that code in `closeCode`): an unmasked
frame to a server or a masked one to a client, reserved bits or opcodes, a
continuation with no message, a control frame over 125 bytes or
fragmented (1002), a text message that is not UTF-8 (1007), a message of
more than 64 MiB, its fragments together (1009). A connection that ends in
the middle of a frame ends without a message. `connect` refuses a server
that chooses a `Sec-WebSocket-Protocol` it was not offered.

```
const websocket = require("@std/websocket")
const ws = websocket.connect("wss://example.org/updates")
ws:send("subscribe")
for (message in ws:messages()) {
	print(message.data)
}
```

### `@std/term`

The terminal. Colours and styles are escape sequences, and they are left
out (the text comes back as it is) when standard output is not a terminal,
`NO_COLOR` is set, or `TERM` is `dumb`; `FORCE_COLOR` keeps them. On
Windows the console is switched to escape sequences when they are first
used.

| function | is |
|---|---|
| `isTerminal(stream?): boolean` | whether `"stdout"` (the default), `"stderr"` or `"stdin"` is a terminal |
| `colors(stream?): boolean` | whether colours go to the stream |
| `size(): { columns, rows }?` | the terminal's size (`nil` when there is no terminal) |
| `style(text, ...): string` | the text in each style named |
| `strip(text): string` | the text without its escape sequences |
| `bold`, `dim`, `italic`, `underline`, `inverse`, `strike`; `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, `gray`, the `bright` ones (`brightRed`, ...), the backgrounds (`bgRed`, ...) | `term.red(text)`: the text in that style |

```
const term = require("@std/term")
print(term.green("ok"), term.style("3 failed", "bold", "red"))
```

### `@std/url`

Web addresses (RFC 3986).

| function | is |
|---|---|
| `parse(text): Url` | `{ scheme, user, password, host, port, path, rawPath, query, fragment }`: the scheme and host in lowercase, `path` decoded and `rawPath` as written (`/a%2Fb` is `path` `/a/b`, `rawPath` `/a%2Fb`), `port` `nil` when the address has none |
| `format(url): string` | the address from its parts: the path as `rawPath` has it while that decodes to `path` (so `%2F` stays `%2F`), else `path` encoded |
| `resolve(base, relative): string` | `relative` read against `base`, as a browser follows a link (`"../c"`, `"?q=1"`, `"//host/x"`), on the paths as written |
| `encode(text, keep?): string` | percent-encoded but for letters, digits and `- . _ ~` (and the characters in `keep`: `"/"` for a path) |
| `decode(text, plus?): string` | `%XX` back to its byte (and `+` to a space, with `plus`) |
| `parseQuery(text): {[string]: string}` | a query string's names and values, decoded |
| `encodeQuery(values): string` | a query string from a table, its names in order |

### `@std/crypto`

Hashes, HMACs and randomness, on mbedTLS (built in). `data` is a string or
a `buffer`; a digest is lowercase hex, or its bytes (a string) with
`options.raw`.

| function | is |
|---|---|
| `hash(algorithm, data, options?): string` | the digest: `"md5"`, `"sha1"`, `"sha224"`, `"sha256"`, `"sha384"` or `"sha512"` |
| `hmac(algorithm, key, data, options?): string` | the HMAC of `data` with `key` |
| `hasher(algorithm): Hasher` | a hash fed in parts: `update(data)` (which returns the hasher, to chain), then `digest(options?)` once |
| `randomBytes(count): string` | `count` random bytes from the operating system (for keys, tokens, salts) |
| `uuid(): string` | a random (version 4) UUID: `"9f1c...-....-4...-....-............"` |
| `equal(a, b): boolean` | whether two secrets are equal, compared in a time that does not depend on where they differ |

```
const crypto = require("@std/crypto")
const fs = require("@std/fs")
print(crypto.hash("sha256", fs.read("release.tar.gz")))

// A message signed with a secret key, and the signature checked.
const key = crypto.randomBytes(32)
const message = "pay 10 to ada"
const signature = crypto.hmac("sha256", key, message)
if (!crypto.equal(signature, crypto.hmac("sha256", key, message))) {
	error("the signature does not match")
}
```

MD5 and SHA-1 are for checksums and the formats that name them; they are
not safe for signatures or passwords.

### `@std/encoding`

Bytes as text, and back. `data` is a string or a `buffer`; what is decoded
is a string of the bytes.

| function | is |
|---|---|
| `toBase64(data, options?): string` | base64; `options.url` for the URL-safe alphabet (`-` and `_`, unpadded unless `options.padding`) |
| `fromBase64(text): string` | the bytes, from either alphabet, padded or not (spaces and line breaks are skipped) |
| `toHex(data): string` | lowercase hex |
| `fromHex(text): string` | the bytes, from hex in either case |

### `@std/compress`

Compression, in memory, as gzip files and HTTP use it (deflate). `data` is
a string or a `buffer`; what comes out is a string of the bytes.

| function | is |
|---|---|
| `gzip(data, options?): string` | gzip's format (what `.gz` files hold); `options.level` from 0 (stored) to 9 (smallest), 6 by default |
| `gunzip(data): string` | the contents of gzip data (of each part, when gzip files were joined), its checksum checked |
| `deflate(data, options?): string` | zlib's format (deflate with its header and checksum); `options.raw` for deflate alone; `options.level` |
| `inflate(data, options?): string` | the contents of zlib data (`options.raw`: of raw deflate) |

### `@std/zip`

Zip archives. An archive is read whole into memory; a new one is built in
memory and written when it is finished.

| function | is |
|---|---|
| `open(path): Archive` | the archive in a file |
| `fromString(data): Archive` | the archive in a string or `buffer` (a download, say) |
| `create(path?): Writer` | a new archive, written to `path` by `finish()` (with no path, `finish()` returns it as a string) |

An `Archive` has `entries()` (each `{ name, size, compressedSize,
directory, modified }`, `modified` a time as `@std/time` has them),
`read(name)` (an entry's contents), `extract(directory)` (every entry under
the directory, made when missing; returns how many files it wrote) and `close()`.
Entry names are matched exactly, case included.
`extract` refuses an entry whose name would put it outside the directory
(an absolute name, or one through `..`), so an archive from anywhere is
safe to extract.

A `Writer` has `add(name, data, options?)`, `addFile(name, path, options?)`
and `addDirectory(name)` (each returns the writer, to chain;
`options.level` as for `@std/compress`), and `finish()`.

```
const zip = require("@std/zip")

const release = zip.create("release.zip")
release:addFile("cluau/README.md", "README.md"):add("cluau/VERSION", "0.24.0\n")
release:finish()

const archive = zip.open("release.zip")
for (_, entry in archive:entries()) {
	print(entry.name, entry.size)
}
archive:extract("unpacked")
```

### `@std/tar`

Tar archives (POSIX ustar, with GNU and PAX long names and link targets
when reading; a name too long for ustar is written as a PAX record),
plain or gzip-compressed: a `.tar.gz` or `.tgz` is found by its first
bytes. The same calls as `@std/zip`:

| function | is |
|---|---|
| `open(path): Archive` | the archive in a file |
| `fromString(data): Archive` | the archive in a string or `buffer` |
| `create(path?, options?): Writer` | a new archive, gzip-compressed when `options.gzip` says so or the path ends in `.gz` or `.tgz` (with no path, `finish()` returns it) |

An `Archive` has `entries()` (each `{ name, size, directory, link, mode,
modified }`; `link` is a link's target), `read(name)`, `extract(directory)`
(refusing an entry that would land outside it; links, devices and FIFOs
are left out) and `close()`. Data that is not an archive (shorter than a
header, a header that does not check, an archive cut short) is an error;
an empty archive has no entries. A `Writer` has `add(name, data, options?)`, `addFile(name, path,
options?)` (`options.mode`, 0644 by default), `addDirectory(name)` and
`finish()`.

### `@std/sqlite`

SQLite databases, with the SQLite library built in (version
`sqlite.version`; full-text search and the math functions included).

| function | is |
|---|---|
| `open(path, options?): Database` | the database in the file (made when missing, unless `options.create` is false); `":memory:"` for one in memory; `options.readonly` opens it read-only |

A `Database` has `path`, and:

| method | is |
|---|---|
| `exec(sql)` | runs the statements in the text (no parameters, no results): a schema, a script |
| `query(sql, ...): {Row}` | every row of the result |
| `first(sql, ...): Row?` | the first row, or `nil` |
| `rows(sql, ...)` | an iterator over the rows, read as the loop goes: `for (row in db:rows(sql)) { }` |
| `run(sql, ...): { changes, lastId }` | runs an `INSERT`, `UPDATE` or `DELETE`: the rows it changed, and the last inserted row's id |
| `prepare(sql): Statement` | the statement compiled once, to run many times: its `query`, `first`, `rows` and `run` take the parameters; `columns()` lists the result's columns; `close()` |
| `transaction(fn, ...)` | `fn(...)` in a transaction: committed when it returns, rolled back when it raises (and the error raised again); nested ones are savepoints. Returns what `fn` returned |
| `close()` | closes the database (its statements too) |

The parameters after the SQL fill its `?`s in order, or one table fills
named ones (`:name`, `@name`, `$name`: the table's keys are the names). A
row is a table of its columns by name: `NULL` is `nil` (so the column is
missing from the table), `INTEGER` and `REAL` are numbers, `TEXT` is a
string and a `BLOB` a `buffer`; a boolean parameter is stored as 1 or 0,
and a buffer as a `BLOB`. A row's type is what the variable it goes to
says it is:

```
const sqlite = require("@std/sqlite")

type User = { id: number, name: string, email: string? }

const db = sqlite.open("app.db")
db:exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT)")
db:run("INSERT INTO users (name, email) VALUES (?, ?)", "ada", "ada@example.com")
const users: { User } = db:query("SELECT id, name, email FROM users WHERE name LIKE ?", "a%")
for (_, user in users) {
	print(user.id, user.name, user.email or "-")
}
db:close()
```

Foreign keys are enforced, and a database another program holds is
waited on for up to five seconds. The calls run on the script's thread
(in a task, the others wait until a query is done).

### `@std/task`

Tasks that run side by side on the script's one thread, for waiting on
several things at once (downloads, programs, timers): each task is a
function run as a coroutine, which runs until it waits, and the others run
meanwhile.

| function | is |
|---|---|
| `spawn(fn, ...): Task` | `fn(...)` as a task, started now: it runs until it first waits |
| `delay(seconds, fn, ...): Task` | `fn(...)` as a task that starts after `seconds` |
| `wait(seconds?): number` | waits `seconds` (none: lets the tasks due now run), and returns how long it waited |
| `join(task): ...` | waits for the task to finish, and returns what its function returned (nothing when it was cancelled) |
| `cancel(task)` | stops the task: it does not run again |
| `running(): Task?` | the task running now (`nil` in the script's own code) |

A `Task` has `done` (it returned, failed or was cancelled) and
`cancelled`. `task.join` is typed by the task's function: joining a task
whose function returns `(string, number)` gives `(string, number)`.

```
const task = require("@std/task")

const http = require("@std/http")

const pages: { task.Task<number> } = {}
for (_, url in { "https://example.com", "https://example.org", "https://example.net" }) {
	table.insert(pages, task.spawn(function(): number {
		return #http.get(url).body // the three downloads run side by side
	}))
}
for (_, page in pages) {
	print(task.join(page))
}
```

- The script's own code waits too: `task.wait` outside a task runs the
  other tasks until its time is up, and `task.join` runs them until the
  task finishes.
- Once the script's last line has run, the tasks still waiting run to
  their end before the script exits.
- An error in a task is raised where the tasks are being run (a
  `task.wait` or `task.join` in the script's code, or the script's end),
  with the task's own stack in the report.
- The standard library's blocking calls wait on their own: `@std/http`'s
  requests and downloads, `@std/net`'s waits, `process.run` and `process.exec`, a child's
  `read`, `readLine`, `lines` and `wait`, `io.readLine`, `io.readAll` and
  `input`, and `process.sleep`. In a task, the call runs on a thread of its
  own while the task waits, and the other tasks run meanwhile; in the
  script's own code, the tasks run until the call is done. So three
  downloads in three tasks take as long as the slowest, not the three
  together.
- Tasks still take turns over the script's own code: a long computation
  without a `task.wait` holds the others up, as do the calls that finish at
  once (files, JSON, paths).

## Embedding the runtime

The runtime is a library (`cluau_runtime`, `include/cluau/runtime.h`):
`cluau::runtime::Runtime` runs a file (`RunFile`), source text
(`RunSource`) or the prompt (`Repl`), and `State()` is its `lua_State` for a
host to add its own globals; `cluau::runtime::Check` is `cluau check`. It
builds against Luau's VM, compiler and analysis: the checkout
`CLUAU_LUAU_DIR` names, else the Luau release cluau pins, which CMake
fetches (`CLUAU_LUAU_VERSION` picks another). `@std/http` adds libcurl
8.23.0 over mbedTLS 3.6.8, fetched and linked in the same way;
`-DCLUAU_HTTP=OFF` builds without it (the README's Building section lists
each module's option). `CLUAU_BUILD_RUNTIME` is on for a standalone build
and off under `add_subdirectory`, so a host that embeds only the translator
needs no Luau.

`Options` sets the script's arguments, the search path, Ctrl-C handling,
native code (`Native`, on by default) and the sandbox (`Sandbox`).

### Host modules

A host gives its scripts modules of its own beside `@std`, under an alias
it chooses: `@<alias>/<name>`, any alias but `std`. A module is made in C++
(a function that returns the module's value) or written in cluau (its
source text); each is loaded on its first `require` and kept.

```cpp
int OpenWorld(lua_State* L)
{
	lua_newtable(L);
	lua_pushcfunction(L, Spawn, "spawn");
	lua_setfield(L, -2, "spawn");
	return 1;
}

cluau::runtime::Runtime runtime(options);
runtime.AddModule("@game/world", OpenWorld);
runtime.AddModuleSource("@game/util", utilSource); // cluau source text
runtime.RunFile("scripts/level.cluau"); // require("@game/world")
```

For `cluau check`, a module made in C++ is described by a Luau file that
returns a value of the module's type; types it declares with `export type`
are named through the module (`world.Entity`):

```luau
export type Entity = { name: string, health: number }
local m: any = nil
return m :: {
	spawn: (name: string) -> Entity,
	find: (name: string) -> Entity?,
}
```

`cluau check --module @game/world=types/world.luau script.cluau` reads it
(a `.cluau` file given that way is typed from its source), as
`CheckOptions::Modules` and `CheckOptions::ModuleSources` do for
`cluau::runtime::Check`.
