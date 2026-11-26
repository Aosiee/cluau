#pragma once

// cluau's lexer: Luau's tokens with cluau's comments (`//`, `/* */`) and
// operators (`!=`, `&&`, `||`, `!`, `~/`, `++`, `--`). Every token keeps its
// byte span and its start / end line and column, so the translator can copy
// the text between tokens verbatim (which keeps the lines).

#include "cluau/cluau.h"

#include <string_view>
#include <vector>

namespace cluau::detail {

	enum class TokenKind : uint8_t
	{
		Name,
		Number,
		String,        // "..." / '...'
		Interpolated,  // `...` (its {} segments are part of the token, unless split)
		InterpBegin,   // split: `...{ (the text before the first segment)
		InterpMid,     // split: }...{ (the text between two segments)
		InterpEnd,     // split: }...` (the text after the last segment)
		LongString,    // [[...]] / [==[...]==]
		LineComment,   // // ... (and a `#!` first line)
		BlockComment,  // /* ... */
		Op,
		Eof,
	};

	struct Token
	{
		TokenKind Kind = TokenKind::Eof;
		std::string_view Text;
		size_t Begin = 0; // byte offsets into the source
		size_t End = 0;
		int Line = 0;
		int Col = 0;
		int EndLine = 0;
		int EndCol = 0;
		bool Open = false; // a comment, long string or interpolated string the source ends inside, or a string it ends while continuing (`\` at a line's end)

		bool Is(std::string_view text) const { return (Kind == TokenKind::Op || Kind == TokenKind::Name) && Text == text; }
		bool IsComment() const { return Kind == TokenKind::LineComment || Kind == TokenKind::BlockComment; }
	};

	// How deeply interpolated strings may nest in one another's `{}`
	// segments (far more than code needs; Luau's parser refuses much less).
	constexpr int kMaxInterpolationNesting = 200;

	// The tokens, ending with one Eof token. Lexical errors (an unterminated
	// string or comment, a stray character) are reported and lexed through.
	// A UTF-8 byte order mark at the start is whitespace. With
	// `splitInterpolation`, an interpolated string with segments is its
	// text pieces (InterpBegin, InterpMid, InterpEnd) with each segment's
	// own tokens between them, as Luau reads it; otherwise it is one
	// Interpolated token.
	std::vector<Token> Lex(std::string_view source, std::vector<Diagnostic>& diagnostics, bool splitInterpolation = false);

} // namespace cluau::detail
