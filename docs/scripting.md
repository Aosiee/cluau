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
A `.luau` file runs as Luau (it is not translated). A script linked from
elsewhere (a symbolic link in a `bin` directory) finds its modules beside
the file it links to. Source text is UTF-8 (a byte order mark at its start
is skipped, in a file, on standard input and in `-e` alike) and cannot
contain a NUL byte.

The exit status is 0, 1 after an error, 2 when the script cannot be read,
130 after Ctrl-C, or the code passed to `process.exit(code)`. Output that
could not be written (`cluau script.cluau > /dev/full`, a full disk) is
reported when cluau exits (`cluau: cannot write standard output`), and a
status of 0 becomes 1; every command does this.

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

- `@std/fs`, `@std/http`, `@std/net`, `@std/sqlite`, `@std/zip`,
  `@std/tar`, `@std/watch` and `@std/worker` (which runs files) are not
  available (`require` raises an error saying so), nor what is built on
  them (`@std/server`, `@std/websocket`).
- `@std/process` keeps `args`, `platform`, `script`, `exit` and `sleep`;
  environment variables, the working directory and running commands are
  left out.
- `require` loads `.cluau` and `.luau` modules only from under the script's
  directory, its `cluau_modules` and the search path (`CLUAU_PATH`).
- `@std/io` (standard input and output), `@std/json`, `@std/time`,
  `@std/path` (text only: `absolute` and `relative` take absolute paths,
  there being no working directory), `@std/regex`, `@std/task`, `@std/crypto`,
  `@std/random`, `@std/encoding`, `@std/compress`, `@std/term`, `@std/url`,
  `@std/args`, `@std/csv`, `@std/toml`, `@std/yaml`, `@std/collections`,
  `@std/uuid`, `@std/semver`, `@std/log` (which has no `CLUAU_LOG` to read
  there) and `@std/test` work as usual, as do Luau's own libraries, which
  have no file or process access.

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
- The `@std/task` tasks a case starts are its own: once its function
  returns, the case waits for them to finish and fails if one of them
  fails (or can never finish). When the case fails first, its tasks are
  cancelled.
- `process.exit` raises an error while the tests run, so code under test
  that calls it (a `--help` handled by `@std/args`, say) fails its case
  instead of ending the run; outside a case it stops the file.
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
(calls and loops), each sample weighted by the time measured since the
last; time inside a long C call (a sleep, a read, a download)
goes to the line that made the call, and so does time in the standard
modules written in cluau (a `task.wait`). The wait at the script's end
for tasks still running counts in the total time but in no function. A
module's top level is
`(top level)`, as the script's is; a coroutine's or a task's function is
`(anonymous)` with its line, unless it has a name. `--top n` shows `n`
functions (20 by default). `--folded file` writes the samples as folded
stacks (`outer;inner count` per line, the count in microseconds), which
flame graph tools (`flamegraph.pl`,
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
disk; a required module's own problems show once it is open), completion
with the types of names, members and the standard modules' members, and
of the paths in `require("...")` (the standard modules, and the modules
and directories beside the script for `./` and `../`), hover (a name's
type and the comment above its declaration, at a use or at the
declaration itself), signature help, go to definition (locals, functions, class
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
modules (a file it cannot read is said on standard error, and the server
starts without that module). A file the editor opens with the `cluau`
language is a cluau script whatever its name (a script without an
extension that starts with a `#!` line naming cluau).

The VS Code extension (in `editors/vscode`; on the VS Code Marketplace and
Open VSX as *cluau*, and as a `.vsix` with each release) starts it, and
adds highlighting, the debugger (see [Debugging](#debugging)) and *Run
File* and *Debug File* commands. It needs `cluau` on `PATH` (or the
`cluau.path` setting; a relative path there, and in `cluau.modules`, is
the workspace folder's). *Run File* runs `cluau` on the file in a terminal
of its own, in the file's directory.

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
  `{expression}` replaced by its value. Each breakpoint on a line counts
  its hits and logs, and a pause names every one that paused there. A
  breakpoint is verified once the script loads its file (until then the
  editor shows it unverified); on a blank or comment line it moves to the
  next line with code, and on a line that only closes a block (`}`) to the
  block's last line with code, so that one on a loop's closing brace
  pauses on each pass. One on a `try`'s `} catch (e) {` line pauses in the
  catch block, when it catches an error, and one on `} finally {` in the
  finally block; steps go through the blocks' statements and never stop on
  those lines. One past the file's last line, one with no code at
  or after it, and one whose condition or hit count does not compile (a
  hit count is at most 2147483647) are not verified, with the reason.
- Pausing on an error that ends the script (on by default), or on every
  error, including those a `try` or `coroutine.resume` catches (*All
  Errors*; not the errors a standard module raises and catches within
  itself, such as a server's read that times out). An error that ends a
  `@std/task` task pauses in the task, with its frames.
- Stepping over, into and out of calls, including into a module's top
  level from its `require`, and into and out of coroutines: a step into
  `coroutine.resume` (or a function `coroutine.wrap` made) stops in the
  coroutine, and a step that leaves it, at a yield or its end, stops where
  it was resumed. A step over `task.wait` (or a blocking call) in a task
  stops at the task's next line once it runs again. Pausing a running
  script, and ending it (*Stop*: it ends as asked, not as a failure).
  `process.exit` ends the script with its status, and the session goes on
  to the editor's end of it.
- Breakpoints, pauses and *Stop* take effect while the script waits in
  `task.wait` or in a blocking call (`process.sleep`, `process.run`, a
  socket's read, a download), with or without `@std/task`; a pause then
  stops at the next line that runs.
- The paused stack: each frame's locals and upvalues, with tables and class
  instances opened field by field (an instance shows its class; a table of
  more than 500 entries shows the first 500 and how many more), and
  expressions (the watch list, hovers, the debug console) evaluated in the
  selected frame, with its locals, upvalues and `...`. An expression
  cannot assign the frame's locals, but it runs for real: a function it
  calls can change the tables the script holds. One that runs longer than
  two seconds (a loop, a slow `__tostring`) is stopped with an error.
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
string are still open continues on the next line (`>>`), as does one that
ends inside an if-expression still waiting for its `else`, or in a string
continued with `\` at the end of the line; an empty line ends it early. A
pasted block arrives whole.

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
is a global too. Errors name the n-th input `stdin[n]`, so a backtrace
through a function defined earlier quotes that input's lines. A value
whose `__tostring` fails is reported as an error, and the session goes on.

- Tab completes globals, keywords, a module's or a table's members
  (`string.up` to `string.upper(`), and an instance's methods after `:`,
  its class's and its base's included.
- Ctrl-C abandons the input being typed (a line, or a statement over
  several) and gives a new prompt; while code runs, it stops the code. On
  an empty prompt, Ctrl-C and Ctrl-D leave.
- The history is kept in `~/.cluau_history`.

A script waiting in `input()` or `io.readLine()` stops at the first
Ctrl-C too (`interrupted`, exit status 130), as does one waiting on a
program it runs (`process.run`, `exec`, a child's reads, writes and `wait`)
or in `process.sleep`; the program is killed. A script started with
Ctrl-C ignored (a shell's background job: `cluau script.cluau &` in a
shell script) leaves it ignored.

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
- `for (name, value in Vec)` goes over the class's own statics and methods
  (not a base class's). `cluau check` types `name` as a string and `value`
  as `unknown` when the loop names a class the file declares; over another
  module's class, annotate them: `for (name: string, value: unknown in
  shapes.Circle)` (Luau's checker otherwise types them `any`).
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
order; a file of any other kind is not a module. A module reached through a
symbolic link resolves its own requires from the directory of the file it
links to (as `cluau check` does). A module that requires itself,
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
| `cluau add <repository>[@ref] [--name <name>]` | adds a dependency and installs it. `@ref` is a tag, a branch or a commit (its id, or the first 7 or more digits of it, pinned as the whole id); without one, the newest version tag (`v1.2.3`), else the default branch. The name is the repository's last part (`my.lib.git` is `my-lib`) unless `--name` gives one. When the install fails, `cluau.toml` is left as it was and the packages it fetched are removed |
| `cluau install` | fetches every dependency at the commit `cluau.lock` pins (resolving and pinning the new ones), and removes what is no longer wanted. A pinned commit it fetches is checked against `cluau.toml` (see below) |
| `cluau update [name...]` | resolves the dependencies' tags and branches again (all of them, or those named) and pins the new commits |
| `cluau remove <name>` | removes a dependency |
| `cluau list` | the installed packages, their commits, and which package wanted each |

- A repository is `github.com/owner/name` (an `https://` address), any Git
  URL (`https://`, `ssh://`, `git@host:path`), or a local directory, in
  `cluau.toml` as in `cluau add`. A relative directory (`../lib.git`) is
  relative to the `cluau.toml` that names it, and stays relative in
  `cluau.lock`. Git fetches them, so `git` must be on `PATH`; private
  repositories use your Git credentials. The `@ref` of `cluau add` is the
  last `@` in the address's path, so a user in it
  (`ssh://git@host/a/b.git@v1`) stays.
- A dependency is `name = { git = "...", tag = "..." }` under
  `[dependencies]`, with at most one of `tag`, `branch` and `rev` (none:
  the default branch). Other keys, a `[dependencies.name]` table and a key
  or table given twice are errors rather than read some other way.
- A package's name is letters, digits, `-` and `_`. An address or a ref
  starting with `-` is refused, in `cluau.toml` and in `cluau add`.
- When `cluau install` fetches the commit `cluau.lock` pins, it checks it
  against what `cluau.toml` names, so that an edited lock cannot install
  a commit the repository does not publish there: for a `tag`, the commit
  must be the one the tag names now; for a `branch` (or the default
  branch), it must be on that branch (its history is fetched to look). A
  `rev` is taken as it is. A mismatch is an error, and `cluau update
  <name>` pins the commit the tag or branch names now. A package already
  installed at the pinned commit is not checked again.
- Packages are written only into a `cluau_modules` directory of the
  project's own: when `cluau_modules`, `cluau.toml` or `cluau.lock` is a
  symbolic link (or a Windows junction), the command refuses rather than
  write or delete where it points. A file a package has under the name
  cluau keeps its commit in (`.cluau-commit`) is replaced, never written
  through.
- `cluau add`, `install`, `update` and `remove` hold `cluau_modules` while
  they run (`cluau_modules/.lock`), and a second one in the same project
  meanwhile stops with an error. If a command was killed, remove that
  directory by hand.
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
| `walk(dir): {string}` | every file under a directory (paths), sorted, names starting with `.` included |
| `mkdir(path)` | creates a directory and its parents |
| `remove(path, all?)` | removes a file or an empty directory; `all` removes a directory's contents too (a symbolic link inside is removed, not followed); a path that is not there is no error |
| `rename(from, to)` | moves; to another file system, a file (or a directory, to a path that is free) is copied there, then removed |
| `copy(from, to)` | copies; a directory recursively, a file onto a directory into it; a directory into itself is an error |
| `size(path): number`, `modified(path): number` | the size in bytes; the last change, in seconds since 1970 |
| `tempDir(): string` | the system's temporary directory (`/tmp` when `TMPDIR` names none) |
| `tempFile(prefix?): string` | a new, empty file of the script's own in the temporary directory (`prefix` is a name, without `/`) |
| `realpath(path): string` | the absolute path with symbolic links, `.` and `..` resolved; the path must exist |
| `glob(pattern): {string}` | the paths matching a pattern, sorted: `*`, `?` and `[a-z]` within a name (`?` is one character, UTF-8 included; Windows compares without case), `**` for any number of directories (`fs.glob("src/**/*.cluau")`); names starting with `.` match only a pattern that does; a pattern ending in `/` matches only directories, and a `[` with no closing `]` is an ordinary character |
| `lines(path)` | the file's lines one by one, for a `for` loop (`for (line in fs.lines("log.txt")) { }`) |
| `open(path, mode?): File` | a file to read (`"r"`, the default), write from empty (`"w"`) or add to (`"a"`); a directory is an error, as for `read` and `lines` |
| `readBuffer(path): buffer`, `writeBuffer(path, bytes)` | the file's bytes as a Luau `buffer`, and back |

A `File` reads and writes as it goes, bytes unchanged:

| method | does |
|---|---|
| `read(count?): string?` | `count` bytes (`nil` at the end; `read(0)` is `""` before it), or without a count everything left |
| `readLine(): string?`, `lines()` | a line without its line break (`nil` at the end); every line, for a `for` loop |
| `write(...)` | writes its arguments (text or numbers) |
| `seek(offset?, from?): number` | moves to `offset` from `"set"` (the start, the default), `"current"` or `"end"`; the position |
| `flush()`, `close()` | a file closes by itself once nothing refers to it; `close` does it now |

A failure is an error naming the call, the path and why, in the system's
words (`fs.read: cannot read 'x.txt': no such file or directory`); `pcall`
it to handle it. A write the system refuses (a full disk) is an error too:
at the call for `write`, `append` and `writeBuffer`, and at `flush` or
`close` for a `File`, which keeps what it writes until then. A path holding
a NUL byte is an error, in `@std/fs` and `@std/path` alike.

### `@std/path`

`join(...)`, `dirname(p)`, `basename(p)`, `stem(p)` (the name without its
extension), `extension(p)` (with its dot: `".txt"`; the dots a name starts
with are the name's, so `.bashrc` has none), `normalize(p)` (`a/./b/../c`
is `a/c`), `absolute(p)`, `relative(p, base?)` (from the current directory
by default), `isAbsolute(p)`. `normalize`, `absolute` and `relative` leave
no `/` at the end (but for a root's), and a path with nothing left is `.`,
so `basename(absolute("."))` is the current directory's name. Paths come
back with `/` on every platform, Windows included (which accepts them).

### `@std/watch`

Changes to files and directories as they happen, for a script that
rebuilds on save or keeps a folder in sync. Each system's own
notifications are used: inotify on Linux, FSEvents on macOS and
`ReadDirectoryChangesW` on Windows (on other systems `new` is an error).
The sandbox leaves the module out.

| function | is |
|---|---|
| `new(path, options?): Watcher` | a watcher of `path`: a directory and everything under it (`options.recursive = false`: only its own entries), or a file; a path that is not there is an error naming it |

A `Watcher` has `path` (the path watched, as given, without a `/` at its
end), `closed` (true once it has stopped: closed, or its directory
removed) and:

| method | is |
|---|---|
| `next(seconds?): Event?` | the next change, waiting up to `seconds` for one (none: until there is one; 0: only one already there); `nil` when the time is up, and at once when the watcher has stopped and nothing is left |
| `events()` | an iterator over the changes, `for (event in w:events()) { }`, which ends when the watcher stops |
| `close()` | stops watching; a wait on the watcher in a task ends with `nil`. A watcher nothing refers to closes by itself |

An `Event` (`watch.Event`) is `{ path: string, kind: watch.Kind, from: string? }`:

| `kind` | means |
|---|---|
| `"created"` | `path` was made (a file or a directory), or moved in from outside what is watched |
| `"modified"` | a file's contents or attributes changed (a directory is never reported as modified) |
| `"removed"` | `path` was removed, or moved out of what is watched |
| `"renamed"` | what was at `from` was renamed to `path`, both under what is watched; the one kind with `from` |
| `"overflow"` | changes came faster than they could be kept, and some were lost: `path` is the watched path, to be looked over again (with `fs.walk`, say) |

A changed path is the watched path joined with the path below it, with `/`
between names on every platform, as `@std/path` and `@std/fs` give them:
watching `"site"`, a change to `main.css` in its `css` directory is
`site/css/main.css`. Watching a file, its events have the file's path as
given.

```
const watch = require("@std/watch")
const process = require("@std/process")

// Runs the tests each time a source file is saved.
const w = watch.new("src")
for (event in w:events()) {
	if (event.kind != "removed" and string.find(event.path, "%.cluau$") != nil) {
		print(`{event.kind}: {event.path}`)
		process.exec({ process.executable, "test" })
		// A save comes as a few changes in a row, and the tests may write
		// files too: what piled up meanwhile is let go.
		while (w:next(0.2) != nil) {
		}
	}
}
```

What a script can rely on, and what differs by system:

- The changes made after `new` returns are reported, each path's in the
  order they happened (but for what the points below say). A change made
  just before may be reported too (FSEvents is a moment behind).
- Notifications for the same path are merged while the script has not yet
  taken them: another of the same kind is not added, nor a `"modified"`
  for a file whose `"created"` is still unread. So a file written in many
  steps gives one event, or a few when the script takes them as they come:
  an event says to look at the path now, not how many times it changed.
- Editors save in different ways: writing the file in place (`"modified"`),
  writing a temporary file and renaming it over the file (`"created"` for
  the temporary file, then `"renamed"` with the file as `path`), or
  removing the file and writing it again (`"removed"`, then `"created"`).
  Where what matters is that a file may have new contents, take
  `"created"`, `"modified"` and `"renamed"` alike.
- A file is watched through its directory, for its name, so all three
  saves are seen; its `"renamed"` events are those to or from its name. A
  watched file that is removed gives `"removed"` and the watcher goes on
  (the file may come back, as `"created"`).
- Removing the watched directory (or the directory of a watched file)
  gives a `"removed"` event for the watched path, and then the watcher
  stops: `closed` is true, and `next` gives what is left, then `nil`.
- On Linux and Windows a rename's two names come from the system as a
  pair. FSEvents reports each name on its own, and the watcher pairs them
  by which one is there now. A rename that cannot be paired, and a move
  into or out of what is watched, are `"created"` and `"removed"`.
- FSEvents (macOS) merges what happens to a path within a moment into one
  notification, and the order of its kinds is worked out from whether the
  path is there now; it may report a change to a file made a moment
  before as `"created"` again.
- Windows reports a file's change as several notifications (its contents,
  its times), merged when unread.
- On Linux each directory under a watched one takes one of the system's
  watches (`fs.inotify.max_user_watches`), and each watcher one inotify
  instance (`fs.inotify.max_user_instances`, often 128); past either
  limit, `new` is an error saying so. A directory made under a watched one
  is watched once it is reported, and what it holds by then is reported as
  created; a file made and removed in it before then is not seen.
- A watcher keeps up to 65536 events the script has not taken; past that,
  an `"overflow"` stands for those it let go.
- A symbolic link may be watched (its target is); the links inside a
  watched directory are reported as entries, not followed.
- Files on a network share may change without a notification (from other
  machines), as each system's documentation warns.
- `next` and `events` wait as the standard library's other blocking calls
  do (see [`@std/task`](#stdtask)): in a task, the task waits on its own
  and the others run meanwhile; in the script's own code, the tasks run
  until a change comes. A Ctrl-C ends the wait.

### `@std/process`

| member | is |
|---|---|
| `args: {string}` | the script's arguments |
| `script: string` | the script's path (`"-e"`, `"stdin"`; `""` at the prompt) |
| `platform: string` | `"linux"`, `"macos"`, `"windows"` or `"other"` |
| `executable: string` | the path of the running `cluau` (to start another script with the same one) |
| `env(name): string?`, `setEnv(name, value?)` | an environment variable (`nil` unsets it; `""` is a value); a name is not empty and has no `=` |
| `environment(): { [string]: string }` | every environment variable |
| `cwd(): string`, `chdir(dir)` | the working directory |
| `run(command, options?): { code: number, output: string, error: string, timedOut: boolean }` | runs a command to its end, capturing its standard output |
| `exec(command, options?): (number, boolean)` | runs a command with the script's own output; its exit code, and whether it timed out |
| `spawn(command, options?): Child` | starts a command that runs alongside the script |
| `sleep(seconds)` | waits (`math.huge`: until Ctrl-C; NaN is an error) |
| `exit(code?)` | ends the script with exit status `code`, a whole number from 0 to 255 (0 by default) |

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
| `env: { [string]: string }` | variables set on top of the script's environment (a name is not empty and holds no `=`; no NUL byte in a name, a value or `cwd`) |
| `stderr` (run only) | `"inherit"` (the default: it goes where the script's goes), `"capture"` (into `error`) or `"merge"` (into `output`, in order) |
| `timeout: number` (run and exec) | seconds it may take; past them the command, and anything it started, is killed and `timedOut` is true. A command that has ended by then finished in time (`timedOut` is false) even if what it started still holds its output open: that is killed too, and the run returns |

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

On Windows the program is looked for in the system's directories
(`System32`, then the Windows directory), then on `PATH` (the child's, when
`env` sets it), trying the extensions in `PATHEXT` (`{ "npm", "install" }`
finds `npm.cmd`); the current directory is not searched unless the name
says so (`.\tool.exe`). A batch file (`.bat`, `.cmd`) runs through `cmd.exe`, and
its arguments are quoted so that each reaches it as it is, with nothing in
them (`&`, `|`, `%NAME%`) taken as `cmd.exe`'s syntax; an argument with a
line break cannot be passed to one, and is an error.

A program still running when the script exits while waiting on it (a
`run` or a child's read in a task, at `process.exit` or an error) is
killed; a child the script is not waiting on is left to run. The script's
own open files and sockets are not passed on to the programs it starts:
they get their standard input, output and error and nothing else.

`spawn` is for a command whose output the script reads while it runs (a
build's progress, a server's log) or one it talks to. It takes `cwd`, `env`,
`stderr = "inherit" | "merge"`, and `input = true` to write the command's
standard input. The `Child` it returns:

| method | does |
|---|---|
| `readLine(): string?`, `lines()` | the next line of its output (`nil` once it has ended); every line, for a `for` loop |
| `read(): string?` | whatever output there is next |
| `write(...)`, `closeInput()` | sends text to its standard input; ends it |
| `wait(): number` | waits for it to end (closing its input first), keeping any output left for `read`; its exit code (each `wait` gets it) |
| `kill()` | ends it, and anything it started |

Tasks may use one `Child` at once: reads share its output out between them
(each line goes to one reader), and every `wait` gets the exit code. On
Linux and macOS, a child that reads the script's terminal (no `input =
true`, with the script's standard input a terminal) stays in the terminal's
process group so that its reads work and Ctrl-C reaches it; `kill()` and a
`timeout` then end the child itself but not what it started.
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
string?` reads a line (without its newline, or the CR before it; `nil` at
the end of input); `readAll(): string` reads the rest; `flush()`. Bytes go
through unchanged, on Windows too when the input or output is a file or a
pipe. A write the system refuses (a full disk, a closed pipe) is an error,
at `write` or at `flush`.

### `@std/json`

`encode(value, pretty?): string` and `decode(text, options?): unknown`.
`pretty` is `true` (two spaces), a number of spaces, or an indent string. Objects' keys are
written sorted, so the output is the same every run; a table whose keys are
exactly `1..n` is an array (a table with gaps, or other keys too, is an
object), and an empty table is `[]`. A value with no JSON form (a function,
a table that contains itself, NaN) is an error, and so is a table with two
keys that would be the same JSON key (`[1]` and `["1"]`), and a string that
is not UTF-8 (JSON text is: bytes go through `encoding.toBase64` first).
`decode` reads arrays and objects up to 1000 inside one another, and reads
a `\u` escape of a lone surrogate (half a pair) as U+FFFD.

- `null` decodes to `nil`, so a key whose value is `null` is absent and an
  array's `null` leaves a gap: `[1,null,3]` encodes again as the object
  `{"1":1,"3":3}`. `decode(text, { null = json.null })` decodes it to
  `json.null` instead, which encodes as `null` again, so the array stays one.
- `json.object(t?)` and `json.array(t?)` mark a table (or a new one) to
  encode as an object or an array: `json.object()` is `{}`, not `[]`. An
  empty object decodes marked, so it encodes as `{}` again.

### `@std/toml`

TOML 1.0.0 (toml.io): configuration files read into tables and written
back.

| function | is |
|---|---|
| `decode(text): {[string]: unknown}` | the document's tables and values. A malformed document, or one that defines a key or a table twice (or adds to an inline table, or gives a `[header]` to a table dotted keys made), is an error saying what and where: `toml.decode: [a] is defined twice (line 3, column 1)` |
| `encode(value, options?): string` | a table as TOML: its values as `key = value` lines, then its tables as `[name]` sections and its arrays of tables as `[[name]]` ones. Keys are sorted, so the output is the same every run, and quoted when they are not bare. `options.multiline` writes a string holding a line break as a `"""` string |
| `datetime(text): DateTime` | a date, a time or both from TOML text: `1979-05-27T07:32:00-08:00`, `1979-05-27 07:32:00`, `1979-05-27`, `07:32:00.5` (`time.iso`'s output is one) |
| `array(t?)` | marks a table (or a new one) to encode as an array: an empty table is otherwise a table |

Strings, booleans, arrays and tables are Luau's; integers and floats are
both numbers. Dates and times are `DateTime` tables, frozen, with
`kind`, one of `"offsetDateTime"` (`1979-05-27T07:32:00Z`),
`"localDateTime"`, `"localDate"` and `"localTime"`, and the fields that
kind has: `year`, `month`, `day`, `hour`, `minute`, `second` (with its
fraction, to the nanosecond), and for an offset date-time `offset`
(seconds east of UTC, as `time.parts` has it) and `time` (the moment, as
`@std/time` has times). `tostring` gives one's TOML text, and two are `==`
when that text is the same.

```
const fs = require("@std/fs")
const time = require("@std/time")
const toml = require("@std/toml")
const config = toml.decode(fs.read("config.toml"))
const server = config.server :: { host: string, port: number }
const started = config.started :: toml.DateTime
print(server.host, server.port, time.iso(started.time))
fs.write("config.toml", toml.encode(config))
```

- An integer outside -2^53 to 2^53 is an error: a number holds integers
  exactly only that far, where TOML allows 64 bits. `encode` writes a whole
  number in that range as an integer (so `1.0` reads back as `1`) and any
  other number so that it reads back the same: `0.1`, `1e+300`,
  `9007199254740994.0`, `-0.0`, `inf`, `nan`.
- Fractions of a second past the nanosecond are cut off, as TOML says; a
  second of 60 is not a time.
- An empty array decodes marked as `toml.array` marks one, so it encodes as
  `[]` again; an unmarked empty table encodes as a table (`[name]`, or `{}`
  inside an array or an inline table).
- A table holding only tables has no header of its own (its sections name
  it). An array whose items are all tables is written as `[[name]]`
  sections; any other array is written on one line, its tables inline
  (`[1, "two", { three = 3 }]`).
- What `encode` cannot write is an error naming where it is: a function or
  another value with no TOML form (`toml.encode: a.b[3] is a function,
  which TOML has no form for`), a table that holds itself, a table whose
  keys are neither all text nor 1 to n, text that is not UTF-8.
- The text must be UTF-8. Line breaks may be `\r\n`, read as `\n` (in
  multi-line strings too), and a byte order mark at the start is passed
  over. Arrays and inline tables nest up to 1000 deep.
- It is TOML 1.0: an inline table is on one line with no comma after its
  last value, a time has its seconds, and `\e` and `\xHH` are not escapes.

### `@std/yaml`

YAML 1.2 (yaml.org/spec/1.2.2): configuration files and data read into
tables and written back.

| function | is |
|---|---|
| `decode(text): unknown` | the text's one document (`yaml.null` when it has none). Several documents are an error, and so is a malformed one, saying what and where: `yaml.decode: the key 'port' is in the mapping twice (line 5, column 3)` |
| `decodeAll(text): {unknown}` | every document of the text, in order (`---` starts one, `...` ends one) |
| `encode(value, options?): string` | a value as a document in block style: keys sorted, so the output is the same every run; strings quoted when they would read as something else and written as literal blocks (`\|`) when they hold line breaks. `options.indent` is the indentation step, 2 (the default) to 9 |
| `null` | what null is: `json.null`, so a key whose value is null stays in its table and `json.encode` writes it as `null` |
| `object(t?)`, `array(t?)` | `json.object` and `json.array`: mark a table to encode as a mapping or a sequence |

Values are read with the 1.2 core schema: `null`, `Null`, `NULL`, `~` and
nothing are null; `true` and `false` (also `True`, `TRUE`, `False`,
`FALSE`) are booleans; integers are decimal (`012` is twelve), `0o17` or
`0x1F`; floats are `1.5`, `.5`, `1e3`, `.inf`, `-.inf` and `.nan`; any
other plain scalar is a string, YAML 1.1's `yes`, `no`, `on`, `off`,
`1_000`, `12:30` and `2001-12-14` among them. Quoted and block scalars are
strings. A mapping decodes marked as `json.object` marks a table, so that
one whose keys are 1 to n encodes as a mapping (or a JSON object) again;
an empty sequence decodes marked as `json.array` marks one.

```
const fs = require("@std/fs")
const yaml = require("@std/yaml")
const config = yaml.decode(fs.read("config.yaml")) :: { server: { host: string, port: number }, tags: { string } }
print(config.server.host, config.server.port, #config.tags)
fs.write("config.yaml", yaml.encode(config))
```

- Everything YAML 1.2 has: block mappings and sequences (with the compact
  forms `- a: 1` and `- - x`, and a sequence at its key's column), explicit
  `? key` entries, flow collections (`[a, b]`, `{a: 1, b}`, a `key: value`
  pair in a sequence), plain scalars over several lines, single- and
  double-quoted ones with every escape, literal (`|`) and folded (`>`)
  block scalars with chomping (`-`, `+`) and indentation indicators,
  comments, `---` and `...`, `%YAML` and `%TAG` directives.
- Keys are scalars, read like values: `1: a` has the number key 1 and
  `"1": a` the text key `"1"`. A key that is a sequence or a mapping (a
  complex key), a null or NaN key, and a key given twice in a mapping are
  errors.
- An alias (`*name`) is the node its anchor (`&name`) names: the same
  table, not a copy. Aliases may stand for 1,000,000 nodes in all (an alias
  counts as the nodes it names, its own aliases included), which stops a
  document of aliases of aliases from growing exponentially; past that, and
  for an alias inside the node it names, `decode` raises an error.
- Tags: `!!str`, `!!int`, `!!float`, `!!bool` and `!!null` read the
  scalar as that type (`!!int "12"` is 12; `!!int abc` is an error),
  `!!seq` and `!!map` check the node is one, and `!` alone makes a scalar a
  string. Any other tag (`!local`, `!!binary`, `!<tag:example.com,2000:x>`)
  keeps the node as it is: a scalar is its text as a string, a collection
  is a sequence or a mapping. `%TAG` handles are expanded before this.
- `%YAML 1.x` is accepted (a 1.1 document is read by the 1.2 rules, so
  `yes` is text); another major version is an error. Other directives are
  passed over.
- An integer outside -2^53 to 2^53 is an error, as in `@std/toml`.
  `encode` writes a whole number in that range as an integer and any other
  number so that it reads back the same, with a point for YAML 1.1 readers:
  `0.1`, `1.0e+300`, `9007199254740994.0`, `-0.0`, `.inf`, `.nan`.
- `encode` quotes a string that would read as something else here or in
  YAML 1.1 (`"yes"`, `"1.0"`, `"null"`, `"012"`, `"12:30"`), one with spaces
  at either end, `: ` or ` #` in it, or an indicator first (`"- x"`,
  `"&a"`), and writes control characters as escapes. An empty table is
  `{}` unless `json.array` marks it; a table met twice is written twice (no
  anchors).
- What `encode` cannot write is an error naming where it is: a function or
  another value with no YAML form (`yaml.encode: a.b[3] is a function,
  which YAML has no form for`), a table that holds itself, a key that is
  not text, a number or a boolean, text that is not UTF-8.
- The text must be UTF-8, with no control characters outside quoted
  escapes (tabs and line breaks aside). Line breaks may be `\r\n` or `\r`,
  read as `\n`, and a byte order mark at the start is passed over.
  Collections nest up to 1000 deep. Indentation is spaces; a tab may
  separate a value from its indicator (`key:<tab>value`).
- Where libyaml (and so PyYAML) and the 1.2 rules differ, this follows the
  rules, but for two cases it reads as libyaml does: a block scalar's
  indentation indicator at the top level counts from column 0 (`--- |2`
  is indented two spaces), and a leading empty line with more spaces than
  a block scalar's first line sets its indentation rather than being an
  error. A block collection does not start on the line of `---`
  (`--- a: b` is an error, as in libyaml).

### `@std/time`

A time is a number: seconds since 1970-01-01 UTC, with a fraction, from the
year 0 to the year 9999 (a time outside them, or NaN, is an error). The
functions that read or write calendar fields take `{ utc = true }`;
without it they use the system's local time zone. They round a time to the
millisecond.

| function | is |
|---|---|
| `now(): number` | the current time |
| `monotonic(): number` | seconds on a clock that only moves forward, for measuring how long something takes |
| `parse(text): number` | an ISO 8601 date or date and time: `2026-11-24`, `2026-11-24T21:05`, `2026-11-24 21:05:09.25+01:00`, `...Z`. A date alone is UTC midnight; a time without an offset is local. An offset is `+hh`, `+hhmm`, `+hh:mm` or `+hh:mm:ss`, under 24 hours; second 60 only ends a day (`23:59:60`) |
| `iso(t?, options?): string` | `2026-11-24T21:05:09.250+01:00` (`...Z` in UTC); an offset with seconds keeps them (`+00:19:32`, local mean time before about 1900), so `parse` reads back the same time |
| `format(t, pattern, options?): string` | `strftime`'s conversions, those C defines and every platform has: `%Y-%m-%d %H:%M:%S`, `%A`, `%B`, `%a %b %c %C %D %e %F %g %G %h %I %j %n %p %r %R %t %T %u %U %V %w %W %x %X %y %z %Z %%`; another (`%-d`, `%k`, `%s`) is an error. In UTC `%z` is `+0000` and `%Z` is `UTC` |
| `parts(t?, options?)` | `{ year, month, day, hour, minute, second, millisecond, weekday, yearday, offset }`: `weekday` 1 is Monday, `offset` is seconds east of UTC |
| `fromParts(parts, options?): number` | the time those fields name (all but `year` optional); a field past its range carries over, so day 32 is the next month's first. `second` and `millisecond` may have a fraction; the others are whole numbers |

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

const release = http.get("https://api.github.com/repos/odd-fish-society/cluau/releases/latest", {
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
(seconds for the whole request, its redirects included), `followRedirects`
(true by default, up to 10), `caFile` (a certificate bundle to trust
instead of the system's). A body goes with its own `Content-Type` header;
without one, none is sent. A header name or a method that is not an HTTP
token, a header value with a line break or a NUL, and an address with a NUL
are errors (they could add headers to the request); a header with an empty
value is sent empty.

A redirect is followed as RFC 9110 has it: a 303 with `GET` (a `HEAD`
stays one), a 301 or 302 after a `POST` with `GET` too, each without the
body; a 307 or 308 keeps the method and the body. A redirect to another
host (or scheme, or port) takes none of the request's own headers but
`Accept`, `Accept-Language`, `Content-Type` and `User-Agent`, and no user
name or password: the others may be credentials meant for the first host.

A `Response` is `{ status, ok, headers, body, url }`: `ok` is a 2xx status,
`headers` has lower-case names (one value per name, repeated ones joined
with `, `), and `url` is the address the answer came from after
redirects. A 4xx or 5xx status is a response, not an error; a request that
gets no answer (an unknown host, a refused connection, a timeout, a
certificate that is not trusted) is an error naming the request (its
address without a password) and why. `download` onto a directory is an
error. Ctrl-C stops a request in progress.

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
  `--help` do). A flag or option declared with `short = "h"` takes `-h`
  for itself (alone or combined), leaving the help `--help` only. `help`
  cannot be declared as a flag or option's name. A required positional
  cannot follow an optional one, and a `number` option's `default` must be
  a number. An argument like `-5` or `-1.5` is a negative number (a
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
| `equal(actual, expected, message?)` | equal values; tables are equal when they hold the same keys with equal values (keys compare as `==` does, so a table as a key is that very table) |
| `notEqual(actual, unexpected, message?)` | anything but `unexpected` |
| `ok(value, message?)` | a value other than `false` or `nil` |
| `near(actual, expected, tolerance?, message?)` | numbers (not strings that look like them) within `tolerance` (`1e-9` by default); NaN is near nothing |
| `raises(fn, text?): string` | `fn` raises an error (containing `text`, when given); its message (an error that is not a string, such as a table, is shown by its contents) |
| `fail(message)` | fails the case here |

### `@std/csv`

Comma-separated values, quoted as RFC 4180 has it (a field holding the
separator, a quote or a line break is quoted, with `""` for a quote):

| function | is |
|---|---|
| `parse(text, options?): {{string}}` | every row's fields (a byte order mark at the start is left out; a blank line is a row of one empty field) |
| `records(text, options?): {{[string]: string}}` | the rows after the first, keyed by the first row's names; blank lines are passed over (a line holding `""` is a record) |
| `encode(rows, options?): string` | rows (of text, numbers or booleans) as CSV, each line ending in `\n`; a `nil` in a row is an empty field, and a row of one empty field is written `""` (an empty line is no row to most readers) |
| `encodeRecords(records, columns, options?): string` | a header row of `columns`, then each record's values in that order (`""` for a missing one) |

`options.separator` is another one-character separator (`";"`, `"\t"`).

### `@std/collections`

What Python's `sorted`, `min`, `max`, `collections` and `itertools` give,
for Luau's lists (items 1 to `#list`, no `nil` among them) and tables.
Every function returns a new list and leaves the one it is given as it
was. The types are generic: `collections.sortBy(people, ...)` is a list of
people, and `collections.set({ "a" })` a `Set<string>`.

| function | is |
|---|---|
| `sorted(items, less?): {T}` | the items sorted, stably (equal items keep their order): by `less(a, b)` (whether `a` comes first), or without it by `<` on items that are all numbers or all strings |
| `sortBy(items, key, options?): {T}` | the items sorted stably by `key(item)`, a number or a string (all of one kind), worked out once for each item; `options.descending` puts the largest first, equal keys still in their order |
| `minBy(items, key): T?`, `maxBy(items, key): T?` | the item with the smallest or largest key, the first of those that tie (`nil` for no items) |
| `counter(items): {[T]: number}` | how many times each item appears |
| `mostCommon(items, n?): {Count<T>}` | `{ item, count }` for each different item, the most frequent first, those as frequent in the order they first appear; the first `n` of them (Python's `Counter(items).most_common(n)`) |
| `groupBy(items, key): {[K]: {T}}` | the items in lists by `key(item)`, each list in the items' order |
| `unique(items): {T}` | each different item once, where it first appears |
| `chunk(items, size): {{T}}` | the items in lists of `size`, the last shorter when they do not divide evenly |
| `zip(a, b)` | an iterator over two lists side by side, as far as the shorter goes: `for (i, x, y in collections.zip(xs, ys)) { }` |
| `flatten(lists): {T}` | the items of each list in turn, one level deep |
| `range(from, to?, step?): {number}` | the numbers `for (i = from, to, step)` goes through (`to` included when a step lands on it); `range(n)` is 1 to `n` |
| `reverse(items): {T}` | the items from last to first |
| `slice(items, from?, to?): {T}` | the items from `from` to `to`, both included, counted as `string.sub` counts: `-1` is the last item; `from` is 1 and `to` -1 by default; bounds past the ends stop there, and `from` after `to` gives `{}` |
| `map(items, fn): {U}` | `fn(item)` for each item; `fn` returning `nil` is an error |
| `filter(items, keep): {T}` | the items `keep(item)` is true for |
| `reduce(items, fn, initial): A` | `fn(...fn(fn(initial, items[1]), items[2])..., items[n])`; `initial` for no items |
| `some(items, test?): boolean`, `every(items, test?): boolean` | whether `test(item)` is true for any item, for every item (`every` is true for no items); without `test`, whether any item, every item, is other than `false` |
| `sum(numbers): number` | the numbers added in order (0 for none) |
| `find(items, test): (T?, number?)` | the first item `test` is true for, and its index |
| `indexOf(items, item): number?` | the index of the first item `==` to `item` |
| `keys(t): {K}`, `values(t): {V}` | a table's keys, sorted: numbers first in order, then strings in order, then `false` and `true`, then any others in the table's own order; its values in the order of its keys |
| `set(items?): Set<T>` | a set of the items, each once, in the order they first appear |
| `deque(items?): Deque<T>` | a double-ended queue holding the items, the first at the front |

A `Set<T>` keeps its members in the order they were added:

| method | is |
|---|---|
| `add(item): boolean` | adds the item; whether it was new |
| `remove(item): boolean` | removes the item; whether it was there |
| `has(item): boolean`, `size(): number` | |
| `items()`, `toList(): {T}` | the members in order, for a `for` loop and as a list; the loop goes over the members as they were when it began, so it may add and remove |
| `union(other)`, `intersection(other)`, `difference(other)`, `symmetricDifference(other)`: `Set<T>` | new sets: this set's members in their order, then (for `union` and `symmetricDifference`) the other's |
| `isSubset(other): boolean` | whether every member is in `other` |

A `Deque<T>` adds and takes items at either end in constant time:
`pushFront(item)`, `pushBack(item)`, `popFront(): T?` and `popBack(): T?`
(`nil` when it is empty), `peekFront(): T?` and `peekBack(): T?` (the item
without taking it), `size()`, and `items()` and `toList()` from front to
back. `print` and `tostring` show either container with its contents
(`Set {"a", "b"}`).

```
const collections = require("@std/collections")

type Person = { name: string, age: number }
const people: { Person } = { { name = "Ada", age = 36 }, { name = "Bo", age = 25 }, { name = "Cy", age = 36 } }

for (_, p in collections.sortBy(people, function(p: Person): number { return p.age }, { descending = true })) {
	print(p.name) // Ada, Cy, Bo
}
const words = { "b", "a", "b", "c", "a", "b" }
print(collections.mostCommon(words, 1)[1].item) // b
print(#collections.chunk(collections.range(10), 3)) // 4
const tags: collections.Set<string> = collections.set()
tags:add("new")
```

- Sorting is stable everywhere (Luau's `table.sort` is not): sorting by
  one key and then by another orders by the second, ties by the first.
  Keys that mix numbers and strings, NaN, and (for `sorted` without
  `less`) items that are not numbers or strings are errors.
- A generic function takes its types from its arguments, so an empty set
  or deque gets its type from where it goes:
  `const seen: collections.Set<string> = collections.set()`.
- NaN cannot be a table key, so it is an error as an item of `counter`,
  `mostCommon`, `unique` and a set, and as a `groupBy` key; `nil` is an
  error as a set member, a deque item and a value `map` returns.
- `chunk`'s size is a whole number from 1, `mostCommon`'s `n` one from 0,
  `slice`'s bounds whole numbers, and `range`'s bounds finite numbers with
  a step other than 0.

### `@std/log`

Structured logging: a record is a level, a message and fields (names with
values), written as one line. By default records go to standard error as
text, from `info` up:

```
const log = require("@std/log")

log.info("started", { port = 8080, root = "/srv/my site" })
// 2026-11-26T21:00:00.123Z INFO  started port=8080 root="/srv/my site"

const db = log.logger("db")
const query = db:with({ file = "app.db" })
query:warn("slow query", { ms = 812 })
// 2026-11-26T21:00:00.125Z WARN  [db] slow query file=app.db ms=812

log.configure({ level = "debug", format = "json" })
query:debug("opened")
// {"time":"2026-11-26T21:00:00.126Z","level":"debug","logger":"db","message":"opened","file":"app.db"}
```

| function | is |
|---|---|
| `debug(message, fields?)`, `info`, `warn`, `error` | writes a record at that level with the default logger (when the level is enabled) |
| `enabled(level): boolean` | whether a record at `level` would be written (to skip working out fields nobody sees) |
| `configure(options)` | sets how records are written; an option left out keeps its setting |
| `logger(name, options?): Logger` | the logger of that name (the same one each time); `options.level` gives it a level of its own |
| `with(fields): Logger` | the default logger with fields added to every record |

The options:

| option | is |
|---|---|
| `level` | `"debug"`, `"info"` (the default), `"warn"`, `"error"` or `"off"`: records below it are left out |
| `format` | `"text"` (the default) or `"json"`: JSON lines, each an object of `time`, `level`, `logger` (for a named logger), `message`, then the fields |
| `time: boolean` | whether records start with the time (true by default; false for output that must be the same every run) |
| `output` | `"stderr"` (the default), `"stdout"`, or a function given each line, without its line break: `function(line: string) { file:write(line, "\n") }` |

A `Logger` has the same `debug`, `info`, `warn`, `error`, `enabled` and
`with` as methods (`db:info("ready")`), and `setLevel(level?)`. A named
logger follows the configured level until it has one of its own (from
`options.level` or `setLevel`; `setLevel(nil)` goes back to following);
one made by `with` has its parent's name and follows the parent's level.
The format, time and output are the same for every logger.

- A text record is the time (UTC, to the millisecond), the level padded to
  five characters, `[name]` for a named logger, the message, and the
  fields as `name=value` in the order of their names. A value is quoted
  when it is empty or holds a space, a quote, `=` or a control character,
  with `\\`, `\"`, `\n`, `\r`, `\t` and `\x1b` escapes inside the quotes; a
  table is shown as `repr` shows it. Control characters in the message
  and the name are escaped too (`\n`), so each record is one line.
- In JSON, a field named like one of the record's own keys (`level`,
  `time`, ...) gets a `_` in front (`_level`). A value with no JSON form (a
  table that contains itself, a function, NaN) is written as its text, and
  bytes that are not UTF-8 as U+FFFD.
- A log call does not fail on its fields: a value that cannot be shown
  (its `__tostring` raises) is written as its type (`<table>`), and fields
  whose iteration raises give the fields read before it. An error the
  `output` function raises is raised from the call.
- `CLUAU_LOG` sets the level a script starts with (`CLUAU_LOG=debug cluau
  script.cluau`; any case, empty is unset); a level given to `configure`
  wins over it. A value that is not a level is reported on standard error
  once, and `info` is used.
- The levels are types: `log.configure({ level = "verbose" })` and
  `db:setLevel("trace")` are errors for `cluau check`, as are fields that
  are not a table (`log.Level`, `log.Threshold` and `log.Fields` name them).

### `@std/regex`

Regular expressions, as Perl and Python write them (PCRE2): alternation,
classes, quantifiers (greedy and lazy), groups (numbered and
`(?<name>...)`), lookahead and lookbehind, backreferences, `\b`, `\d`,
`\s`, `\w`, and Unicode properties (`\p{L}`). A pattern is compiled once
and matched as UTF-8 text: `\w`, `\d` and case-insensitive matching know
letters beyond ASCII. Text that is not valid UTF-8 is matched where it is
valid: no pattern matches an invalid byte or a match across one, a
lookbehind stops at one, and `^` and `$` (without `m`) do not match beside
one. Positions are 1-based byte
offsets, as `string.find` gives them; a `start` past the end of the text
plus one finds nothing.

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
| `split(text, limit?): {string}` | the pieces between matches (at most `limit` of them); an empty match at the very start or end splits nothing off |

A `Match` is `{ text, start, finish, groups, named }`: the matched text,
where it starts and ends, the groups by number (a group that took no part
is `nil`) and by name.

`matches`, `replace` and `split` find every match as Perl and Python do:
after an empty match, a non-empty one starting at the same place comes
next, if there is one (so `|abc` finds `""`, `"abc"` and `""` in `"abc"`).

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
`(a+)+$`) stops with an error instead of running on: one search may take
ten million steps and 64 MB of memory, and the searches of one
`matches`, `replace` or `split` about ten million steps in all and a
thousand more for each byte of the text. (A group repeated over a long
text, such as `(a|b)*` over a few hundred thousand characters, needs more
memory than that: match the pieces instead, `[ab]*`.)

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
| `readLine(limit?): string?` | the next line, without its line break (`\r\n` or `\n`); the last line may have none; `nil` at the end. With `limit`, a line longer than that many bytes (its line break aside) is an error (and stays unread): a server's guard against a client that never ends its line |
| `lines()` | an iterator over the lines: `for (line in socket:lines()) { }` |
| `readAll(): string` | the rest of the stream, until the other side closes it |
| `write(...)` | sends the strings, in order |
| `setTimeout(seconds?)` | how long each wait of a read or write may take (none, or 0: no limit); a peer that sends a byte now and then never trips it |
| `setDeadline(seconds?)` | the time from now within which every read and write, those under way too, must end, however the data comes (an error then: `the time setDeadline gave has passed`); none: no deadline |
| `close()` | ends the connection (a wait on it in another task ends with an error); what was read before is still given out, then reads are errors |

`tls` (on a socket and a server) says whether it is encrypted.

TLS: `connect`'s `options.tls = true` encrypts the connection, checking
the server's certificate against the system's trusted certificates (or
`options.caFile`'s, a PEM file) and its name against `host` (or
`options.serverName`); `options.verify = false` skips both, for a server
of one's own with a certificate nothing trusts. `listen`'s
`options.tls = { certFile, keyFile }` (PEM files: the certificate chain and
its key, which must be the certificate's) serves TLS. `accept` returns a
connection as soon as it arrives, and its handshake is made by the
connection's first read or write, so a client that is slow to begin holds
up only its own task; a handshake has the socket's timeout (30 seconds
without one) in all; a connection whose handshake fails reads as one that
ended (`nil`), and a write to it is an error.

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
		// A client that leaves early makes a write fail: an error in a task
		// would end the whole script, so the connection's work is in pcall.
		pcall(function() {
			for (line in client:lines()) {
				client:write(string.upper(line), "\n")
			}
		})
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
| `serve(options, handler): Server` | serves `handler` and returns at once; `options.host` (`"127.0.0.1"` by default: this machine only), `options.port` (8080; 0 for any free one), `options.tls` (`{ certFile, keyFile }`), `options.maxBody` (16 MiB), `options.idle` (seconds a connection may wait for its next request, and each wait while reading or writing one, 60), `options.headTimeout` (seconds a request's head, its request line and headers, may take in all from its first byte, 30; 0: no limit; a client that sends a byte now and then is answered 408). The `Server` has `host`, `port` and `close()` (which stops taking connections and closes those waiting for a request; a request under way is finished) |
| `router(): Router` | routes by method and path: `get`, `post`, `put`, `delete` and `route(method, pattern, handler)` (each returns the router, to chain), and `handler()` for `serve`. A pattern's parts are literal, `:name` (in `request.params.name`) or `*` (the rest, in `request.params["*"]`). The path is split at its `/`s before it is decoded, so `/users/a%2Fb` has the one part `a/b` |
| `files(directory): Handler` | serves the files under `directory` (`index.html` for a directory, the content type by extension), each sent as it is read; a path that would leave the directory, or holds a NUL, is refused, and hidden files (a part starting with `.`: `.git`, `.env`) are not found. A symbolic link inside `directory` is followed only to a file that is itself inside `directory` |
| `upgrade(request, fn): Answer` | answers a WebSocket request: `fn` gets the connection (a `@std/websocket` `WebSocket`), and the connection ends when `fn` returns. A request that is not a WebSocket one as RFC 6455 has it (a `GET` of HTTP/1.1 with `Upgrade: websocket`, `Connection: Upgrade` and a `Sec-WebSocket-Key` of 16 bytes in base64) is answered 400, and one of a version other than 13 426; in a build without `@std/crypto`, `upgrade` is an error (so a 500) |

A `Request` has `method`, `path` (decoded, without the query), `query`
(its names and values, decoded), `headers` (names in lowercase), `body`,
`params`, `remoteHost`, `remotePort`, `version` (`"1.1"` or `"1.0"`) and
`target` (as the request line gave it; an absolute one, `http://host/path`,
is routed by its path). A handler answers with a table (`status`, a whole
number from 200 to 999, 200 by default; `headers`, whose values are strings
or numbers; `body`, a string or a buffer), a string (200, as text), or `nil`
(404). An error in a handler is a 500, with the error on stderr; so is any
other answer, and a response header whose name is not a header name or
whose value has a line break. The server frames the answer itself: a
handler's `Content-Length`, `Transfer-Encoding`, `Connection`, `Keep-Alive`,
`TE`, `Trailer` and `Upgrade` are left out (so an answer relayed from
`@std/http` keeps its framing right), and a 204 or 304 is sent without a
body or a `Content-Length`. A large body is written a piece (256 KiB) at a
time, each within `options.idle`, so a client that reads slowly holds little
of the server's memory, and not for long. Nothing a client does ends the
server: an error on a connection ends that connection only.

Requests are read strictly, so that the server and anything in front of it
read the same request: a body is framed by `Content-Length` (digits only,
the same if repeated) or by `Transfer-Encoding: chunked`, not both and no
other coding (400; 501 for a coding before `chunked`), and a body that ends
early never reaches the handler (400, and the connection closes). A line of
the head may have 64 KiB, and the head 256 KiB (431; 414 for the request
line). A header's name is a token, white space around its value is spaces
and tabs only, and a value with a control character (a CR, a NUL) is
refused (400); so is an HTTP/1.1 request without exactly one `Host`, and a
version other than 1.1 and 1.0 is a 505. `Connection` is read as a list
(`close, TE` closes); an HTTP/1.0 request with `Transfer-Encoding` is read
and the connection closed after it. `Expect: 100-continue` is answered with
`100 Continue` before the body is read. After an answer to a request that
was not read to its end, what the client still sends is read and dropped
for half a second at most before the connection closes. A failed `accept`
(out of file descriptors, say) is reported on stderr and tried again after
a pause.

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
| `connect(url, options?): WebSocket` | a connection to a `ws://` or `wss://` address (its `#fragment` is not sent); `options.headers`, `options.protocols` (the subprotocols offered), `options.timeout` (seconds for connecting and the handshake; the connection's waits after it have no limit), and for `wss`, `options.caFile` and `options.verify` as `@std/net` has them |

A `WebSocket` has `send(text)`, `sendBinary(data)`, `ping(data?)`,
`receive()` (the next message, `{ kind, data }` with `kind` `"text"` or
`"binary"`, or `nil` once the connection has closed), `messages()` (an
iterator: `for (message in ws:messages()) { }`), `close(code?, reason?)`,
and `protocol`, `closeCode` and `closeReason` (the other side's, whichever
side closed first). A ping's data and a close's payload are at most 125
bytes; a close's code is one an endpoint may send (1000 to 1003, 1007 to
1014, 3000 to 4999) and its reason UTF-8.

A frame that breaks RFC 6455 closes the connection with the code it gives
(and `receive` returns `nil`, with that code in `closeCode`): an unmasked
frame to a server or a masked one to a client, reserved bits or opcodes, a
continuation with no message, a control frame over 125 bytes or
fragmented, a close code no endpoint sends (1002), a text message or a
close reason that is not UTF-8 (1007), a message of more than 64 MiB, its
fragments together, or in more than 65536 fragments (1009). A connection
that ends in the middle of a frame ends without a message. `connect`
refuses a header name or protocol that is not a token, a header value with
a line break, and an address with white space or a control character in
it (any of them could add to the request); and a server that answers
without `Upgrade: websocket` and `Connection: Upgrade`, with a head of more
than 256 KiB or 100 headers, or with a `Sec-WebSocket-Protocol` it was not
offered. In a build without `@std/crypto`, `connect` is an error.

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
| `strip(text): string` | the text without its escape sequences: CSI (`ESC [`, colours), OSC (`ESC ]`, links), DCS, SOS, PM and APC strings, and the short ones (`ESC ( B`, `ESC 7`, `ESC c`) |
| `bold`, `dim`, `italic`, `underline`, `inverse`, `strike`; `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, `gray`, the `bright` ones (`brightRed`, ...), the backgrounds (`bgRed`, ...) | `term.red(text)`: the text in that style |

```
const term = require("@std/term")
print(term.green("ok"), term.style("3 failed", "bold", "red"))
```

### `@std/url`

Web addresses (RFC 3986).

| function | is |
|---|---|
| `parse(text): Url` | `{ scheme, user, password, host, port, authority, path, rawPath, query, fragment }`: the scheme and host in lowercase, `path` decoded and `rawPath` as written (`/a%2Fb` is `path` `/a/b`, `rawPath` `/a%2Fb`), `port` `nil` when the address has none, `authority` whether it has a `//` part (`http:///x` has one with no host). A port that is not a number from 0 to 65535 is an error |
| `format(url): string` | the address from its parts: the path as `rawPath` has it while that decodes to `path` (so `%2F` stays `%2F`), else `path` encoded; a `//` part when `authority` says so (when it is not given: when there is a host, or for a `file:` address's absolute path) |
| `resolve(base, relative): string` | `relative` read against `base` by RFC 3986's rules (`"../c"`, `"?q=1"`, `"//host/x"`), on the paths as written. They are the strict rules: `http:g` against an `http:` base stays `http:g`, where a browser reads it as `g` |
| `encode(text, keep?): string` | percent-encoded but for letters, digits and `- . _ ~` (and the characters in `keep`: `"/"` for a path) |
| `decode(text, plus?): string` | `%XX` back to its byte (and `+` to a space, with `plus`) |
| `parseQuery(text): {[string]: string}` | a query string's names and values, decoded; pairs are split at `&` only (a `;` is part of a value, as browsers have it) |
| `encodeQuery(values): string` | a query string from a table, its names in order (as text) |

### `@std/uuid`

UUIDs (RFC 9562): identifiers made from the operating system's random
source, which no one can guess and which in practice never come out the
same twice, and read back. A UUID is text: 32 lowercase hex digits in
groups of 8, 4, 4, 4 and 12, joined by hyphens
(`0f8fad5b-d9cb-469f-a165-70867728950e`).

| function | is |
|---|---|
| `v4(): string` | a random UUID (version 4): 122 random bits |
| `v7(): string` | a UUID ordered by time (version 7): the Unix time in milliseconds, then a counter and random bits. Those a script makes sort in the order it made them, as text, even many in the same millisecond, so they suit database keys |
| `parse(text): string?` | the UUID in its canonical form (lowercase): from any case, in braces (`{...}`) or after `urn:uuid:`; `nil` when the text is not a UUID |
| `isValid(text): boolean` | whether `parse` reads it |
| `version(text): number?` | its version, 1 to 8 (0 for `NIL`, 15 for `MAX`); `nil` when the text is not a UUID, or is one of another variant than RFC 9562's, whose bits there are not a version |
| `time(text): number?` | when a version 7 UUID (or a version 1 or 6, which hold a time too) was made, in seconds since 1970-01-01 UTC, as `@std/time` has times; `nil` for other versions |
| `NIL`, `MAX` | `00000000-0000-0000-0000-000000000000` and `ffffffff-ffff-ffff-ffff-ffffffffffff` (upper case, as Python and npm name them: `nil` is a Luau keyword, so `uuid.nil` could not be written) |

```
const uuid = require("@std/uuid")
const time = require("@std/time")

const id = uuid.v7()
print(id, uuid.version(id), time.iso(uuid.time(id) :: number))
const given = uuid.parse("{0F8FAD5B-D9CB-469F-A165-70867728950E}")
if (given == nil) {
	error("not a UUID")
}
```

- `v7` follows RFC 9562's first method for order (section 6.2): a 42-bit
  counter after the time starts at a random number below 2^41 in each new
  millisecond and goes up by 1 for each UUID made in the same one, or
  while the system clock reads earlier than the last time used; the last
  32 bits are random in every UUID. The order holds within one runtime;
  UUIDs from different processes made in the same millisecond have no
  order among them.
- `parse` takes the hyphenated form only: 32 hex digits without hyphens,
  spaces around the text, or braces and `urn:uuid:` together are not a
  UUID.
- Every function but `v4` and `v7` takes text, and other values are an
  error. The random bytes come from the system a few thousand at a time
  (`getrandom` on Linux, `arc4random_buf` on macOS, `BCryptGenRandom` on
  Windows), as `crypto.randomBytes` has them, in every build.

### `@std/semver`

Semantic Versioning 2.0.0 (semver.org): versions read and written, ordered
by precedence, and incremented, and npm's ranges (`^1.2.3`, `~1.2`, `1.x`,
`>=1.2.3 <2 || 3.0.0 - 3.4`), for scripts that make releases or pick
dependencies.

| function | is |
|---|---|
| `parse(text): Version?` | `{ major, minor, patch, prerelease, build }`: the numbers, and the identifiers after `-` and `+` in order, numeric prerelease ones as numbers (`1.0.0-rc.1+linux.64` has `prerelease` `{ "rc", 1 }` and `build` `{ "linux", "64" }`); `nil` when the text is not a version |
| `isValid(text): boolean` | whether `parse` reads it |
| `format(version): string` | a `Version` as text: `1.0.0-rc.1+linux.64` |
| `compare(a, b): number` | -1, 0 or 1 as `a` has lower, the same or higher precedence than `b` |
| `lessThan(a, b)`, `greaterThan(a, b)`, `equals(a, b)`: `boolean` | the same comparisons; `equals` is the same precedence, so `1.0.0+a` equals `1.0.0+b` |
| `sort(list, options?)` | a new list of the versions from lowest to highest (`options.descending`: highest first), those of the same precedence in the order they were in |
| `increment(version, release, identifier?): string` | the next version for a `"major"`, `"minor"`, `"patch"`, `"premajor"`, `"preminor"`, `"prepatch"` or `"prerelease"` release, as npm's `semver.inc` makes it (below) |
| `satisfies(version, range): boolean` | whether the version is in the range |
| `maxSatisfying(list, range)`, `minSatisfying(list, range)` | the highest or lowest version of the list in the range, as the list has it (the first of those of the same precedence); `nil` when none is. Items that are not versions (`"latest"`, `"v1.2.3"`) are passed over |
| `parseRange(text): Range` | the range as `{ sets }`: lists of comparators (`{ operator, version }`, the operator one of `<`, `<=`, `>`, `>=`, `=`), what `~`, `^`, x-ranges, partial versions and hyphen ranges stand for written out. A version is in the range when it satisfies every comparator of one of the sets; a set with none is any version |
| `formatRange(range): string` | a range written out as npm writes one: `^1.2.3` is `>=1.2.3 <2.0.0-0`, sets are joined by ` \|\| `, and a set of any version is `*` |

A version or a range is text or what `parse` or `parseRange` gives; the
list functions give back what their list holds (text from a list of text).
Text that is not a version, a malformed range or a malformed `Version` is
an error saying what is wrong: `semver.satisfies: '1.2.3 <' is not a
range: '<' has no version after it`.

```
const semver = require("@std/semver")

print(semver.compare("1.0.0-rc.1", "1.0.0"))                 // -1
print(semver.increment("1.4.0-beta.2", "prerelease"))       // 1.4.0-beta.3
print(semver.increment("1.4.0", "preminor", "rc"))          // 1.5.0-rc.0
print(semver.satisfies("1.4.2", "^1.2.0 || ~2.0.1"))        // true
const tags = { "1.2.3", "1.9.0", "latest", "2.0.0-rc.1", "2.0.0" }
print(semver.maxSatisfying(tags, "<2"))                     // 1.9.0
print(table.concat(semver.sort({ "1.10.0", "1.2.0", "1.2.0-rc.1" }), " "))   // 1.2.0-rc.1 1.2.0 1.10.0
```

- Versions are SemVer 2.0.0's grammar exactly: three numbers without
  leading zeros, then an optional `-` prerelease and `+` build, each of
  dot-separated identifiers of letters, digits and hyphens (a numeric
  prerelease identifier has no leading zero). A `v` or `=` before the
  version, and spaces around it, are not part of one. The numbers, and
  numeric identifiers, are at most 2^53 - 1, the largest a number holds
  exactly.
- Precedence is the specification's (section 11): the numbers in turn; a
  version with a prerelease below the same version without one; then the
  prerelease identifiers in order, numbers by value, below text, and text
  by its ASCII order (`Z` before `a`), more identifiers above fewer when
  the ones before are the same. Build metadata plays no part.
- Ranges follow npm's grammar. A comparator is `<`, `<=`, `>`, `>=` or `=`
  (or none) and a version, spaces between them allowed; comparators
  separated by spaces must all hold, and sets separated by `||` are
  alternatives. A partial version or one with `x`, `X` or `*` for a number
  stands for all the versions it names: `1.2` and `1.2.x` are
  `>=1.2.0 <1.3.0-0`, `*` (or nothing) any version, `>1.2` `>=1.3.0`,
  `<=1.2` `<1.3.0-0`. A prerelease of `-0` is the lowest there is, so
  `<1.3.0-0` leaves out the prereleases of 1.3.0 as well as 1.3.0.
- `~` keeps the minor version when one is given, the major otherwise:
  `~1.2.3` is `>=1.2.3 <1.3.0-0`, `~1.2` is `>=1.2.0 <1.3.0-0`, `~1` is
  `>=1.0.0 <2.0.0-0`. `^` keeps the left-most part that is not 0:
  `^1.2.3` is `>=1.2.3 <2.0.0-0`, `^0.2.3` is `>=0.2.3 <0.3.0-0`, `^0.0.3`
  is `>=0.0.3 <0.0.4-0`, and of a partial version, the parts given
  (`^0.0` is `>=0.0.0 <0.1.0-0`, `^1.x` is `>=1.0.0 <2.0.0-0`).
- A hyphen range `A - B` is `>=A <=B`, its set's only part, spaces around
  the `-`: a partial version on the left has its missing numbers as 0
  (`1.2 - 2.3.4` is `>=1.2.0 <=2.3.4`), and on the right takes in all it
  names (`1.2.3 - 2.3` is `>=1.2.3 <2.4.0-0`).
- A prerelease version is in a range only when a comparator of the same
  set has a prerelease of the same major.minor.patch, as npm has it:
  `1.2.3-beta.4` satisfies `^1.2.3-beta.2`, while `1.3.0-beta` does not
  satisfy `^1.2.3`, nor `1.2.3-beta` `*`. A prerelease is then something
  a range asks for by name, not an update it takes in. Each set is tried
  on its own, so `1.2.3-beta` satisfies `1.2.3-beta || *`.
- Build metadata in a range is ignored. What npm reads only in its loose
  mode (`v1.2.3`, `~>1.2`, `1.2.3beta`) and its `includePrerelease` option
  are not part of this module.
- `increment` gives text, its build metadata left off. `"major"`,
  `"minor"` and `"patch"` raise that number and set those after it to 0,
  but a prerelease of a version that is already the next one becomes it:
  `1.2.3` and `2.0.0-rc.1` both go to `2.0.0` with `"major"`, `1.2.0-1`
  goes to `1.2.0` with `"minor"`, `1.2.3-4` to `1.2.3` with `"patch"`.
  `"prerelease"` raises the last numeric prerelease identifier
  (`1.2.3-alpha.9.beta` to `1.2.3-alpha.10.beta`), adds `.0` when there is
  none (`1.2.3-beta` to `1.2.3-beta.0`), and of a version without a
  prerelease makes the next patch's first (`1.2.4` to `1.2.5-0`).
  `"premajor"`, `"preminor"` and `"prepatch"` raise that number, set those
  after it to 0 and start a prerelease: `1.2.3` to `2.0.0-0`, `1.3.0-0`,
  `1.2.4-0`. With an `identifier` (one identifier: `"rc"`, `"beta"`) the
  prerelease of the four `pre` releases starts with it: what follows
  `rc.` is kept when it was already `rc.<number>` (`1.2.3-rc.1` to
  `1.2.3-rc.2`), and otherwise it is `rc.0` (`1.2.3-alpha.1` to
  `1.2.3-rc.0`, `1.2.3` to `1.2.4-rc.0`). The identifier is ignored for
  `"major"`, `"minor"` and `"patch"`.

### `@std/random`

Random numbers from a seed, for simulations, games, tests and sampling.
The generator is xoshiro256\*\* (Blackman and Vigna), its state filled from
the seed by splitmix64: a seed gives the same numbers on every platform and
every run. The numbers are not for secrets (keys, tokens, passwords), since
a few of them give away the rest: `crypto.randomBytes` and `crypto.uuid`
(see [`@std/crypto`](#stdcrypto)) are for those.

The module's functions share one generator, seeded from the operating
system when it is first used. `new` makes a generator of the script's own
(a `random.Random`), which has the same functions as methods
(`rng:integer(1, 6)`):

| function | is |
|---|---|
| `new(seed?): Random` | a generator of its own, seeded with `seed` (without one, from the system) |
| `seed(seed?)` | starts the generator over from `seed` (without one, from the system) |
| `random(): number` | a number from 0 up to 1, never 1: one of the 2^53 multiples of 2^-53 there, each as likely |
| `integer(min, max): number` | a whole number from `min` to `max`, both included, each as likely |
| `uniform(a, b): number` | a number between `a` and `b` (`b` itself only through rounding) |
| `normal(mean?, deviation?): number` | a number from the normal distribution with that mean (0) and standard deviation (1) |
| `choice(list): T` | one of the list's items |
| `shuffle(list)` | the list's items put in a random order, in place (Fisher-Yates: every order as likely) |
| `sample(list, count): {T}` | `count` items from different places in the list, in the order drawn |
| `weighted(list, weights): T` | one of the list's items, each as likely as its weight (`weights[i]` for `list[i]`) |

```
const random = require("@std/random")

print(random.integer(1, 6), random.choice({ "rock", "paper", "scissors" }))

const rng = random.new(17) // level 17 is the same every time it is played
const enemies = rng:integer(3, 8)
const loot: string = rng:weighted({ "coin", "gem", "key" }, { 80, 15, 5 })
const order = { "north", "east", "south", "west" }
rng:shuffle(order)
```

- A seed is a whole number from -2^63 to 2^63 (a time such as
  `time.now()` has a fraction: `math.floor` it). What a seed gives is part
  of the cluau version: the same on every platform and every run with that
  version, which is what a test or a saved game relies on; another version
  may change it.
- To repeat a run, seed the shared generator (`random.seed(42)`) or give
  each part of the program its own `new(seed)`: then one part's draws do
  not change another's numbers.
- `integer` takes whole numbers from -2^53 to 2^53 (past them a double does
  not hold every whole number), `min` no greater than `max`, and a range of
  at most 2^53 numbers. It draws as many bits as the range needs and draws
  again when they fall past it, so no number is more likely than another
  (as a remainder would make them).
- `uniform` and `normal` take finite numbers, and the deviation is 0 or
  more (0 gives the mean). `normal` is Kinderman and Monahan's ratio of
  uniforms, as Python's `random.normalvariate`.
- `choice` of an empty list is an error. `sample` takes a count from 0 to
  the list's length. `weighted` takes one weight for each item, each a
  finite number, 0 or more, not all 0; an item of weight 0 never comes up.
- A list is the table's items from 1 to `#list`, read and written without
  metamethods.

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
| `uuid(): string` | a random (version 4) UUID: `"9f1c...-....-4...-....-............"` (as `uuid.v4()` makes, which [`@std/uuid`](#stduuid) has in every build, with time-ordered ones) |
| `equal(a, b): boolean` | whether two secrets (strings or buffers) are equal, compared in a time that does not depend on where they differ |

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
| `fromBase64(text): string` | the bytes, from either alphabet, padded or not (spaces and line breaks are skipped); text that isn't the exact encoding of its bytes (bits left over in the last character, padding that doesn't fill the last group of four) is an error |
| `toHex(data): string` | lowercase hex |
| `fromHex(text): string` | the bytes, from hex in either case |

### `@std/compress`

Compression, in memory, as gzip files and HTTP use it (deflate). `data` is
a string or a `buffer`; what comes out is a string of the bytes.

| function | is |
|---|---|
| `gzip(data, options?): string` | gzip's format (what `.gz` files hold); `options.level` from 0 (stored) to 9 (smallest), 6 by default |
| `gunzip(data): string` | the contents of gzip data (of each part, when gzip files were joined), its checksums checked; a header with flags gzip reserves is refused |
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
Entry names are matched exactly, case included; when two entries have one
name, `read` gives the last, which is also the one `extract` leaves.
An archive from anywhere is safe to open and extract: `extract` refuses an
entry whose name would put it outside the directory (an absolute name, one
through `..`, or one with a `\`), and opening refuses an archive whose
entries share their data or name other files in their local headers, or
whose entries claim more than 1,040 times the archive's size (and 64 KB)
in all. So what `extract` writes, and what reading every entry once gives,
is at most that (deflate itself expands at most about 1,032 times): check
the `entries()` sizes first when even that is too much.

A `Writer` has `add(name, data, options?)`, `addFile(name, path, options?)`
and `addDirectory(name)` (each returns the writer, to chain;
`options.level` as for `@std/compress`), and `finish()`. A name `extract`
would refuse, and a name already in the archive, are errors.

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
(refusing an entry that would land outside it or whose name holds a NUL
byte; links, devices and FIFOs are left out; the files are written with
the usual permissions, not the archive's modes) and `close()`. A GNU
sparse file is listed with its real name and size, but `read` and
`extract` refuse it. Data that is not an archive (shorter than a header, a
header that does not check, an archive cut short, a PAX size that is not
a whole number) is an error; an empty archive has no entries. A `Writer`
has `add(name, data, options?)`, `addFile(name, path, options?)`
(`options.mode`, 0644 by default: a whole number from 0 to 0o7777777),
`addDirectory(name)` and `finish()`.

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
| `rows(sql, ...)` | an iterator over the rows, read as the loop goes: `for (row in db:rows(sql)) { }`. Until the last row is read, the statement holds a read lock on the database (see below) |
| `run(sql, ...): { changes, lastId }` | runs an `INSERT`, `UPDATE` or `DELETE`: the rows it changed, and the last inserted row's id |
| `prepare(sql): Statement` | the statement compiled once, to run many times: its `query`, `first`, `rows` and `run` take the parameters; `columns()` lists the result's columns; `close()`. Running it again while one of its `rows` loops is under way ends that loop with an error |
| `transaction(fn, ...)` | `fn(...)` in a transaction: committed when it returns, rolled back when it raises (and the error raised again); nested ones are savepoints. When the commit itself fails (a deferred foreign key, another connection reading), the transaction is rolled back and the error raised. Returns what `fn` returned |
| `close()` | closes the database (its statements too, which also lets go of their locks) |

The parameters after the SQL fill its `?`s in order, or one table fills
named ones (`:name`, `@name`, `$name`: the table's keys are the names; a
name the table lacks is `NULL`, as a `nil` value is). SQL holding a NUL
byte is an error. A row is a table of its columns by name: `NULL` is `nil`
(so the column is missing from the table), `INTEGER` and `REAL` are
numbers, `TEXT` is a string and a `BLOB` a `buffer`; a boolean parameter
is stored as 1 or 0, and a buffer as a `BLOB`. Two columns of one name
are one key: name them apart with `AS`. A
number is a double, so an `INTEGER` beyond 2^53 (about 9e15) comes back
rounded: read such values with `CAST(x AS TEXT)`. `run` gives 0 `changes`
for a statement that changes no rows. A row's type is what the variable it
goes to says it is:

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
waited on for up to five seconds. A `rows` loop left early (with `break`)
keeps its read lock until the statement is collected, and another
connection that writes meanwhile waits and then fails: use `query` for
results that are read in part, or `prepare` the statement and `close()`
it after the loop (closing the database ends it too). The calls run on the script's thread
(in a task, the others wait until a query is done).

### `@std/task`

Tasks that run side by side on the script's one thread, for waiting on
several things at once (downloads, programs, timers): each task is a
function run as a coroutine, which runs until it waits, and the others run
meanwhile.

| function | is |
|---|---|
| `spawn(fn, ...): Task` | `fn(...)` as a task, started now: it runs until it first waits |
| `async(fn, ...): Task` | `fn(...)` as `spawn` starts it, but an error in it is raised where the task is joined (what an `async function`'s call is) |
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
  with the task's own stack in the report. A task `task.async` started
  (an async function's) keeps its error instead: `task.join` (or `await`)
  raises it where the task is joined, so a `try` there catches it, and one
  that nothing joins is raised once the script's own code has finished.
- An `async function` is a function whose body runs as a task, and `await
  t` is `task.join(t)` (see [the syntax guide](syntax.md#async-functions-and-await)):

  ```
  const task = require("@std/task")

  async function slow(n: number): number {
  	task.wait(0.1)
  	return n * 2
  }

  const a = slow(1) // the two run side by side: 0.1 seconds in all
  const b = slow(2)
  print(await a + await b) // 6
  ```
- The standard library's blocking calls wait on their own: `@std/http`'s
  requests and downloads, `@std/net`'s waits, `process.run` and `process.exec`, a child's
  `read`, `readLine`, `lines`, `write` and `wait`, `io.readLine`,
  `io.readAll` and `input`, a watcher's `next` and `events` (`@std/watch`),
  `process.sleep`, and `@std/worker`'s `receive` and `join`. In a task, the call runs
  on a thread of its own while the task waits, and the other tasks run meanwhile; in the
  script's own code, the tasks run until the call is done. So three
  downloads in three tasks take as long as the slowest, not the three
  together.
- A task cancelled while such a call waits takes nothing from it: what
  a child's, standard input's or a socket's read got goes back for the
  next read, a connection `server:accept` got goes to the next accept
  (or is closed with the server), and a change a watcher's `next` got goes
  to the next wait on the watcher.
- A finished task keeps nothing once nothing refers to it, so a server
  that runs a task for each connection stays the same size.
- Tasks still take turns over the script's own code: a long computation
  without a `task.wait` holds the others up, as do the calls that finish at
  once (files, JSON, paths). For work that needs more than one core, see
  [`@std/worker`](#stdworker).

### `@std/worker`

Scripts that run in parallel, for work that needs more than one core. A
worker is a script file run in a runtime of its own (its own Luau VM, its
own modules and globals) on a thread of its own: the script that started it
and the worker share nothing, and what they send each other is copied.

| function | is |
|---|---|
| `start(path, ...): Worker` | the script at `path` running as a worker, with `...` (copied) as its `worker.args` |

A `Worker` is the starting script's side of it:

| member | is |
|---|---|
| `send(value)` | sends the worker a copy of `value` (dropped once the worker has ended) |
| `receive(seconds?): unknown` | the next value the worker sent; `nil` when `seconds` pass first, or once the worker has ended and its values are read |
| `join(): ...` | waits for the worker to end, and returns what its script returned; an error in the worker is raised here |
| `terminate()` | stops the worker at its next safepoint |
| `done: boolean` | the worker has ended (returned, failed or was terminated) |

In the worker's own script, the module talks to the script that started it:

| member | is |
|---|---|
| `isWorker: boolean` | `true` in a worker (`false` in the main script) |
| `args: {unknown}` | what `start` was given after the path (`{}` in the main script) |
| `send(value)` | sends the starting script a copy of `value` |
| `receive(seconds?): unknown` | the next value the starting script sent; `nil` when `seconds` pass first, or once it can send no more (its script has ended, or its `Worker` was collected) and the values are read |

```
// main.cluau
const worker = require("@std/worker")

const parts: { worker.Worker } = {}
for (i = 0, 3) {
	table.insert(parts, worker.start("count", i * 25000000 + 1, (i + 1) * 25000000))
}
let primes = 0
for (_, part in parts) {
	primes += part:join() :: number // the four count side by side, on four cores
}
print(primes)
```

```
// count.cluau
const worker = require("@std/worker")

const from = worker.args[1] :: number
const to = worker.args[2] :: number
let count = 0
for (n = from, to) {
	// ... count the primes ...
}
return count
```

- `path` is relative to the calling script's directory, as
  `require("./path")` takes it (the `.cluau` or `.luau` may be left off),
  or absolute. The worker's runtime has the standard modules, the script's
  arguments (`process.args`) and its search path; `process.script` is the
  worker's file. A host's own modules (`Runtime::AddModule`) are not in
  it, since they may hold the host's state.
- What crosses (the arguments, the values sent, the results): `nil`,
  booleans, numbers, strings (any bytes), buffers, `json.null`, and tables
  of these, keys too, read without metamethods. A table marked by
  `json.object` or `json.array` keeps its mark; any other table with a
  metatable (an instance of a class) is refused, as are functions,
  threads and userdata (a `Worker` too), a table inside itself, a table
  met twice in one value, and tables more than 1000 inside one another.
  The error says where: `worker:send: a function at value.handlers[2]
  cannot be sent`. `send(nil)` is refused too, since `receive` gives `nil`
  for no value.
- The values sent one way arrive in the order sent. There is no limit on
  how many wait to be received: a worker that sends faster than its
  values are read holds them in memory.
- `receive` and `join` wait as the other blocking calls do (see
  [`@std/task`](#stdtask)): in a task, the task waits and the other tasks
  run meanwhile; in the script's own code, the tasks run until the call is
  done. A task cancelled while it waits takes nothing: the value is the
  next receive's. `receive(0)` looks without waiting.
- `join` gives the results again when called again. An error in the worker
  (in its code or in one of its tasks) is raised by `join` with the
  worker's message and stack:

  ```
  main.cluau:4: the worker count.cluau failed: count.cluau:9: attempt to compare nil < number
  stack backtrace (in the worker):
  ...
  ```

  A worker that `terminate` stopped gives `join` nothing, and so does one
  whose script called `process.exit(0)`: in a worker, `process.exit` ends
  the worker's script, not the process, and a status other than 0 is an
  error for `join`.
- `terminate` takes effect at the worker's next safepoint (a call or a
  loop's next round, as Ctrl-C does); a worker waiting in a blocking call
  that Ctrl-C ends (`worker.receive`, `process.sleep`, a download, a task's
  wait) stops waiting. A `catch` in the worker sees the error, but the
  next safepoint raises it again, so the worker ends. Values it sent
  before are still received.
- When the starting script's code and its tasks have finished, the workers
  still running are waited for, as tasks are: their `worker.receive` gives
  `nil` once they have read what was sent, so a worker looping on it ends.
  The first error of a worker that nothing joined is then the script's
  error. When an error ends the script instead, its workers are
  terminated. `process.exit` in the main script stops every worker before
  the process ends.
- Ctrl-C stops the workers too, each at its next safepoint, and the script
  exits with status 130.
- A worker can start workers of its own; its script's end waits for them.
- `print` and `io.write` write each call's text at once, so the lines of
  workers printing at the same time do not mix; their order is the order
  the writes happen.
- The debugger, the profiler and `cluau test --coverage` see the main
  script, not its workers. `@std/worker` is not available in
  [the sandbox](#the-sandbox), since it runs files.

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

The runtime sets Luau's interrupt callback on `State()` for each run; one
the host had set there is called first, so a host's watchdog keeps
working. Several runtimes can live in one process, each on one thread at a
time, and each profiles on its own; Ctrl-C (`CatchInterrupt`) is the
process's signal, and stops whichever catching runtime reaches its next
safepoint first. A file or source run from inside a run (a host's callback
calling `RunFile`) leaves the outer script's directory and
`process.script` as they were when it returns. A script's workers
([`@std/worker`](#stdworker)) are runtimes the runtime makes, each on a
thread of its own, without the host's modules or interrupt callback:
`RunFile` and `RunSource` return once the workers still running have
ended (and after terminating them, when the script failed), and destroying
the runtime terminates any left (those the prompt started). `cluau::runtime::Repr`
runs a value's `__tostring`, which can raise a Lua error: call it where
an error is caught (inside a protected call).

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
