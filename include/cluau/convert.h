#pragma once

// cluau convert - standard Luau to cluau (docs/syntax.md "Converting").
// The conversion is syntactic and keeps every line on its line: block words
// become braces (`if c then` -> `if (c) {`, `end` -> `}`, `repeat ... until c`
// -> `do { ... } while (!(c));`), `local` becomes `let`, the operators and
// comments take their cluau spelling, and Luau's `--!` mode comments are
// dropped (cluau hosts decide the mode). It does not restructure code: a
// module table stays a module table (turning one into a `class` is the
// author's call, because the class shape is the host's).
//
// What has no cluau form is reported as an error and left as written: an
// if-expression, and an identifier that is a cluau keyword (`let`, `const`,
// `class`).

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
	// tokens with `luau`'s (comments, parentheses and doubled `not`s
	// ignored). Returns "" when they match, else the first difference.
	std::string VerifyConversion(std::string_view luau, std::string_view converted);

} // namespace cluau
