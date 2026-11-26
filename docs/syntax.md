# cluau syntax

cluau is **C-syntax-flavoured Luau**: braces and C operators on top of
Luau's types, semantics and standard library. A `.cluau` file is translated
to standard Luau before the Luau type checker and compiler see it. The
translation keeps every line on its own line, so a line number in a Luau
error or a debugger is the line in the `.cluau` file.

Anything not described here is Luau unchanged: the type syntax (`: number`,
`type`, generics, unions, `?`), method calls with `:` (`obj:Method()`),
strings and string interpolation, `#`, `..`, 1-based arrays, `nil`,
`return`, `break`, `continue`, and the compound assignments (`+=`, ...).

## Statements and blocks

| cluau | Luau |
|---|---|
| `if (c) { ... } else if (d) { ... } else { ... }` | `if c then ... elseif d then ... else ... end` |
| `while (c) { ... }` | `while c do ... end` |
| `for (i = 1, 10) { ... }` | `for i = 1, 10 do ... end` |
| `for (k, v in t) { ... }` | `for k, v in t do ... end` |
| `do { ... } while (c);` | `repeat ... until not (c)` |
| `{ ... }` as a statement | `do ... end` |
| `function f(a: number): number { ... }` | `function f(a: number): number ... end` |
| `let x = 1` | `local x = 1` |
| `const x = 1` | `local x = 1`, and `x` may not be assigned again |

- Conditions are parenthesised: `if (x > 0) { ... }`.
- A `{` opens a block after a statement header (`if (...)`, `while (...)`,
  `for (...)`, `else`, `do`, a function's signature) and at the start of a
  statement. Anywhere else it is a table constructor, as in Luau.
- Luau's call sugar stays: `f "text"` and `f { ... }` are calls. A `{` on
  the same line right after an expression is a table argument. A `{` that
  starts a line after a finished statement is a block; inside parentheses,
  brackets or a table constructor a `{` is always a table.
- A statement ends where its last value or type does: after `x++`, after
  `...`, and after a type such as `let best: string?` or
  `type T = Array<number>`, the next line starts a new statement.
- `local` is accepted as a synonym for `let` (also in `for (local i = 1, n)`).
- Luau's block words (`then`, `end`, `elseif`, `repeat`, `until`) are not
  used; writing one is an error.

## Operators

| cluau | Luau |
|---|---|
| `!=` | `~=` |
| `&&`, `\|\|` | `and`, `or` |
| `!x` | `not x` |
| `a ~/ b`, `a ~/= b` | `a // b`, `a //= b` (floor division) |
| `x++`, `x--` (statements) | `x += 1`, `x -= 1` |

`and`, `or` and `not` are also accepted.

### If-expressions

An `if` where an expression is expected yields a value:

```
let speed = if (boosted) 2 else 1
let label = if (n == 0) "none" else if (n == 1) "one" else "many"
```

The condition is parenthesised and there are no braces; `else` is
required. It is Luau's if-expression (`if c then a elseif d then b else e`):
only the branch taken is evaluated, and unlike `c && a || b` it works when
`a` is `false` or `nil`. The last branch extends as far as an expression
can, so `if (c) 1 else 2 + x` adds `x` to the `else` branch only. C's
`c ? a : b` is not cluau: `?` and `:` already mean optional types,
annotations and method calls.

## Errors: try, catch, finally, throw

```
function Load(file: string): Config? {
	try {
		return json.decode(fs.read(file)) :: Config
	} catch (error) {
		if (!fs.exists(file)) {
			return nil
		}
		throw error
	} finally {
		print("read", file)
	}
}
```

- `try { }` runs its block; when an error escapes it, `catch (name) { }`
  runs with the error value as `name`. Either `catch` or `finally` may be
  left out, not both.
- The caught value is `unknown` to the type checker (an error can be any
  value); `catch (error: string)` declares it.
- `finally { }` runs after the rest whatever happened: after the block, after
  the catch, and before an error that neither handled travels on.
- A `return`, `break` or `continue` inside `try` or `catch` does what it
  says, from where the statement stands: `return` leaves the function
  (after `finally`), `break` and `continue` act on the loop around the
  statement. A `finally` block cannot return, break or continue.
- `throw value` raises `value` as it is: a re-raised error keeps its
  message and position. It works anywhere, including inside
  `catch (error)`, where the name `error` is the caught value rather than
  Luau's function, and wherever else a local named `error` hides it.
- `try` and `catch` blocks see their function's `...`.
- `try`, `catch`, `finally` and `throw` are keywords only where they start
  those statements; elsewhere they are ordinary names.

It lowers to `pcall`, every line on its line.

## Enums

```
enum Color { Red, Green, Blue }
enum Status {
	Ok = 200
	NotFound = 404
}

let c: Color = Color.Red
```

- A member without a value is its own name as a string (`Color.Red ==
  "Red"`): it prints, compares and serialises as itself. A member's value
  may be a string or a number instead.
- The enum's type is the union of its string members (`"Red" | "Green" |
  "Blue"`), so a name that is not a member is a type error; an enum of
  numbers is `number` (Luau has no number literal types), and one with
  both is its strings' union and `number` (`enum M { A = "x", B, C = -1 }`
  is `"x" | "B" | number`).
- The enum is a frozen table: `for (name, value in Status) { }` lists its
  members, and assigning to it is an error.
- Members are separated by commas or line breaks.

## Comments

| cluau | Luau |
|---|---|
| `// line` | `-- line` |
| `/* block */` | `--[[ block ]]` |

`--` is the decrement operator, never a comment.

A `//!` comment before the file's code would be a Luau mode comment
(`--!nonstrict`): it is an error, since the host decides the checking mode.
Anywhere else `//!` is an ordinary comment.

## Classes

A file may declare one class. It must be the file's last statement; the
file's module value is the class. (A script that `cluau` runs may declare
any number, anywhere at its top level, and construct them by calling them:
see [scripting.md](scripting.md#classes-in-scripts).)

```
class Player {
	@export Speed: number = 4        // a property the host exposes (an inspector field)
	VelocityY: number = 0            // a declared field
	GroundY: number = 0

	function OnStart() {
		self.GroundY = self.Entity.Transform.Position.y
	}

	function OnFixedUpdate(dt: number) {
		if (self:Grounded()) {
			self.VelocityY = 0
		}
	}

	function Grounded(): boolean {
		return self.Entity.Transform.Position.y <= self.GroundY
	}
}
```

- A field is `Name: Type [= default]`, one per line. Attributes before it
  (`@export`, ...) are passed to the host's class profile. The default is
  translated like any other expression (a `function (...) { ... }` value
  included) and may continue on the next lines while a bracket is open.
- A method is a `function` in the class body. Its `self` is the class type,
  added for you.
- The class type has exactly the declared fields and methods (plus the
  host's base members): using an undeclared field is a type error.
- What `@export` means, and which base members `self` has, depend on the
  host that runs the script (its *class profile*). TinyEngine's profile
  makes `@export` fields inspector properties and gives `self` an `Entity`.
- `static Name = expression` is a member of the module table, not of an
  instance (the class's own `Name`, shared by every instance). It is how a
  class carries tables a host reads from the module, e.g. TinyEngine's
  `static Server = { Reset = ... }`.
  A static's expression is translated like any other (braces, operators);
  a `function (...) { ... }` value inside it is an ordinary function.
- `signal Name(param: Type, ...)` declares a signal the class emits, with
  its payload's types (every parameter typed). The names become the module
  table's signal list (`Class.Signals = { "Name", ... }`, the member the
  host's profile names); a host uses the declarations to type its emit and
  connect calls - TinyEngine types every `Emit`, `On` and `Multicast` in
  the workspace from them, so an undeclared name or a wrong payload is a
  type error. A class with signal declarations has no `static Signals`.

  ```
  class Coin {
  	signal Collected(position: vec3)

  	function OnStart() {
  		World.Emit("Collected", self.Entity.Transform.Position)
  	}
  }
  ```
- A host may spell a field's type differently in Luau (its profile's
  `FieldType`): TinyEngine's `AssetRef<Scene>` field is a `string` (the
  asset's path) to the type checker, and an asset picker in the inspector.
- A host whose instances carry their signals as objects sets its profile's
  `SignalType`, and each declared signal becomes a member of the class type
  (TinyEngine: `Collected: Signal<vec3>`, so `self.Collected:Emit(...)`
  checks against the declaration).

## Strict typing

Hosts may enforce cluau's strict rules (`cluau_check`):

1. Every module is checked in Luau's strict mode.
2. `any` is an error wherever it appears in an annotation or a cast, or
   flows into a value the script uses, unless that line carries an
   exemption comment: `// allow-any: <reason>` (the reason is required).
3. Function parameters are annotated unless their type comes from
   context. A function that returns a value annotates its return type: a
   declared one, one assigned to a `const` or `let`, and a function
   expression with no type from context (a callback's comes from the
   parameter it is passed to).
4. `self` has only the declared fields.
5. A `const` local is never assigned again.

An exemption that covers nothing is a warning.

## Manifest mode

A host may translate in *manifest mode* (Kiln's build manifests):
`function`, `while`, `do { } while`, `try` and `class` are rejected, and
the file is declarations only.

## Converting

`cluau convert [--verify] <file.luau>` rewrites standard Luau as cluau,
keeping every line on its line: block words become braces, conditions get
parentheses, `local` becomes `let`, the operators and comments take their
cluau spelling (`--` comments become `//`, `--[[ ]]` becomes `/* */`).
`repeat ... until c` becomes `do { ... } while (!(c));`, however many
lines the condition takes. It does not restructure code: a module table
stays a module table. Luau's `const x = 1` and `const function f()` stay
as they are (cluau's `const` is the same). A byte order mark is kept.

Mode comments before the code (`--!strict`, `--!native`, `--!optimize 2`,
...) are dropped: cluau hosts decide the mode. cluau accepts no `//!` at
the start of a line, so any other `--!` comment there becomes `// !`.

An if-expression becomes cluau's (`if a then b elseif c then d else e`
-> `if (a) b else if (c) d else e`), in an interpolated string's `{}`
segments too. A comment inside a segment is dropped (with a warning). A
call whose table argument starts the next line (`f` then `{ x = 1 }`)
gets its parentheses (`f(` then `{ x = 1 })`), since in cluau that `{`
would open a block; for the same reason a do-block after a value or a
type on its line (`local x: number? do`) is written `; {`.

What has no cluau form is reported as an error and left as written: an
identifier that is a cluau keyword (`let`, `class`, and `const` other than
Luau's declaration); `if local` and `while local`; a function, or a long
string holding braces, quotes or backticks, inside an interpolated
string's segment; and a call to a function named `throw` or `try` where
cluau reads a statement's start (at the start of a line, or after `;` or
a block's brace: `throw(x)` there would read as cluau's `throw`).

`--verify` translates the result back and checks it is the input's tokens
again, each on its line (comments, parentheses, `;` and doubled `not`s
aside; an if-expression's `then` may move to the condition's line). The
`cluau` tool then parses the input and the round trip with Luau's parser
and checks they are the same program (parentheses and doubled `not`s
aside), so a passing `--verify` means the result translates back to Luau
that parses to the input's program. Input that Luau does not parse fails
it. A host that embeds only the translator gets the token check
(`VerifyConversion`), which cannot see brackets in the wrong place.

