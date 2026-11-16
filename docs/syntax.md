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
  starts a line after a finished statement is a block.
- `local` is accepted as a synonym for `let`.
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

## Comments

| cluau | Luau |
|---|---|
| `// line` | `-- line` |
| `/* block */` | `--[[ block ]]` |

`--` is the decrement operator, never a comment.

## Classes

A file may declare one class. It must be the file's last statement; the
file's module value is the class.

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
  (`@export`, ...) are passed to the host's class profile.
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
  `static Signals = { "Collect" }` or `static Server = { Reset = ... }`.
  A static's expression is translated like any other (braces, operators);
  a `function (...) { ... }` value inside it is an ordinary function.

## Strict typing

Hosts may enforce cluau's strict rules (`cluau_check`):

1. Every module is checked in Luau's strict mode.
2. `any` is an error wherever it appears in an annotation or a cast, or
   flows into a value the script uses, unless that line carries an
   exemption comment: `// allow-any: <reason>` (the reason is required).
3. Function parameters are annotated unless their type comes from
   context. A function that returns a value annotates its return type.
4. `self` has only the declared fields.
5. A `const` local is never assigned again.

An exemption that covers nothing is a warning.

## Manifest mode

A host may translate in *manifest mode* (Kiln's build manifests):
`function`, `while`, `do { } while` and `class` are rejected, and the file
is declarations only.

## Converting

`cluau convert [--verify] <file.luau>` rewrites standard Luau as cluau,
keeping every line on its line: block words become braces, conditions get
parentheses, `local` becomes `let`, the operators and comments take their
cluau spelling (`--` comments become `//`, `--[[ ]]` becomes `/* */`), and
`--!` mode comments are dropped. `repeat ... until c` becomes
`do { ... } while (!(c));`. It does not restructure code: a module table
stays a module table.

Two things have no cluau form yet and are reported, left as written: an
if-expression (`local x = if a then b else c`) and an identifier that is a
cluau keyword (`let`, `const`, `class`).

`--verify` translates the result back and checks it is the input's tokens
again (comments, parentheses and doubled `not`s aside).

