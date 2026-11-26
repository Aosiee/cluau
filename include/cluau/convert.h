#pragma once

// cluau convert - standard Luau to cluau (docs/syntax.md "Converting").
// The conversion is syntactic and keeps every line on its line: block words
// become braces (`if c then` -> `if (c) {`, `end` -> `}`, `repeat ... until c`
// -> `do { ... } while (!(c));`), `local` becomes `let`, the operators and
// comments take their cluau spelling, and Luau's `--!` mode comments
// before the code are dropped (cluau hosts decide the mode). It does not
// restructure code: a module table stays a module table (turning one into
// a `class` is the author's call, because the class shape is the host's).
//
// An if-expression becomes cluau's `if (c) a else if (d) b else e`. What
// has no cluau form is reported as an error and left as written: an
// identifier that is a cluau keyword (`let`, `class`, `const` other than
// Luau's `const` declaration), `if local` / `while local`, and what an
// interpolated string's segment cannot hold in cluau (a function, a long
// string with braces, quotes or backticks).

#include "cluau/cluau.h"

#include <string>
#include <string_view>
#include <vector>

namespace cluau {

	struct ConvertResult
	{
		std::string Cluau;
		std::vector<Diagnostic> Diagnostics; // source (Luau) positions
		bool HasErrors() const;
	};

	ConvertResult Convert(std::string_view luau);

	// Checks a conversion: translates `converted` back and compares its
	// tokens with `luau`'s (comments, parentheses, `;` and doubled `not`s
	// ignored), and that each keeps its line. Returns "" when they match,
	// else the first difference. The tokens can match where the brackets
	// do not, so this alone does not show the result compiles: with Luau
	// at hand, parse `roundTrip` (the Luau it translates back to) and the
	// input, and compare the two (`cluau convert --verify` does).
	std::string VerifyConversion(std::string_view luau, std::string_view converted, std::string* roundTrip = nullptr);

} // namespace cluau
