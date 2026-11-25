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
| `cluau add <repository>[@ref] [--name <name>]` | adds a dependency and installs it. `@ref` is a tag, a branch or a commit; without one, the newest version tag (`v1.2.3`), else the default branch. The name is the repository's last part unless `--name` gives one |
| `cluau install` | fetches every dependency at the commit `cluau.lock` pins (resolving and pinning the new ones), and removes what is no longer wanted |
| `cluau update [name...]` | resolves the dependencies' tags and branches again (all of them, or those named) and pins the new commits |
| `cluau remove <name>` | removes a dependency |
| `cluau list` | the installed packages, their commits, and which package wanted each |

- A repository is `github.com/owner/name` (an `https://` address), any Git
  URL (`https://`, `ssh://`, `git@host:path`), or a local directory. Git
  fetches them, so `git` must be on `PATH`; private repositories use your
  Git credentials.
- A package is a repository whose `init.cluau` is what `require("name")`
  returns; `require("name/part")` loads `part.cluau` from it.
- A package's own `cluau.toml` dependencies are installed too, beside it in
  the same `cluau_modules/`. Two packages that want one name from different
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
`pretty` is `true` (two spaces) or an indent string. Objects' keys are
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
- Short flags combine (`-vq`); `-h` and `--help` print the help.
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
| `near(actual, expected, tolerance?, message?)` | numbers within `tolerance` (`1e-9` by default) |
| `raises(fn, text?): string` | `fn` raises an error (containing `text`, when given); its message |
| `fail(message)` | fails the case here |

### `@std/csv`

Comma-separated values, quoted as RFC 4180 has it (a field holding the
separator, a quote or a line break is quoted, with `""` for a quote):

| function | is |
|---|---|
| `parse(text, options?): {{string}}` | every row's fields |
| `records(text, options?): {{[string]: string}}` | the rows after the first, keyed by the first row's names |
| `encode(rows, options?): string` | rows (of text, numbers or booleans) as CSV, each line ending in `\n` |
| `encodeRecords(records, columns, options?): string` | a header row of `columns`, then each record's values in that order |

`options.separator` is another one-character separator (`";"`, `"\t"`).

## Embedding the runtime

The runtime is a library (`cluau_runtime`, `include/cluau/runtime.h`):
`cluau::runtime::Runtime` runs a file (`RunFile`), source text
(`RunSource`) or the prompt (`Repl`), and `State()` is its `lua_State` for a
host to add its own globals; `cluau::runtime::Check` is `cluau check`. It
builds against Luau's VM, compiler and analysis: the checkout
`CLUAU_LUAU_DIR` names, else the Luau release cluau pins, which CMake
fetches (`CLUAU_LUAU_VERSION` picks another). `@std/http` adds libcurl
8.23.0 over mbedTLS 3.6.8, fetched and linked in the same way;
`-DCLUAU_HTTP=OFF` builds without it. `CLUAU_BUILD_RUNTIME` is on for a standalone build
and off under `add_subdirectory`, so a host that embeds only the translator
needs no Luau.
