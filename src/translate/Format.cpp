// cluau fmt: indentation from the brackets open at each line's start,
// trailing whitespace removed, one final line break. Only the whitespace
// around a line's text changes, and none inside a token that spans lines.

#include "cluau/format.h"
#include "Lexer.h"

#include <algorithm>

namespace cluau {

	using detail::Token;
	using detail::TokenKind;

	namespace {

		bool IsOpener(const Token& t) { return t.Is("(") || t.Is("[") || t.Is("{"); }
		bool IsCloser(const Token& t) { return t.Is(")") || t.Is("]") || t.Is("}"); }

		// Ends a line that the next line continues: an operator still waiting
		// for its right side.
		bool AwaitsOperand(const Token& t)
		{
			if (t.Kind == TokenKind::Name)
				return t.Is("and") || t.Is("or") || t.Is("not");
			if (t.Kind != TokenKind::Op)
				return false;
			static constexpr const char* kOps[] = { "=", "+", "-", "*", "/", "%", "^", "..", "==", "!=", "~=", "<", "<=", ">", ">=", "&&",
				"||", "~/", "+=", "-=", "*=", "/=", "%=", "^=", "..=", "~/=", "->", "|", "&" };
			for (const char* op : kOps)
				if (t.Text == op)
					return true;
			return false;
		}

		// Starts a line that continues the one before: a member access, a
		// method call, or a binary operator (not one that can begin an
		// expression, such as `-` or `!`; Luau has no unary `+`).
		bool ContinuesLine(const Token& t)
		{
			if (t.Kind == TokenKind::Name)
				return t.Is("and") || t.Is("or");
			if (t.Kind != TokenKind::Op)
				return false;
			static constexpr const char* kOps[] = { ".", ":", "..", "+", "*", "/", "%", "^", "==", "!=", "~=", "<=", ">=", "&&", "||", "~/" };
			for (const char* op : kOps)
				if (t.Text == op)
					return true;
			return false;
		}

	} // namespace

	bool FormatResult::HasErrors() const
	{
		return std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const Diagnostic& d) { return !d.Warning; });
	}

	FormatResult Format(std::string_view source, const FormatOptions& options)
	{
		// A byte order mark stays where it is, before the first line's indentation.
		constexpr std::string_view kBom = "\xEF\xBB\xBF";
		if (source.substr(0, kBom.size()) == kBom)
		{
			FormatResult result = Format(source.substr(kBom.size()), options);
			result.Text.insert(0, kBom);
			result.Changed = result.Text != source;
			return result;
		}
		FormatResult result;
		std::vector<Token> tokens = detail::Lex(source, result.Diagnostics);
		if (result.HasErrors())
		{
			result.Text = std::string(source);
			return result;
		}

		// The lines, and each one's line break: every line keeps its own
		// (a CRLF in a long string is part of the string), and a last line
		// that had none gets the one most lines use.
		std::vector<std::string_view> lines;
		std::vector<uint8_t> crlf;
		size_t crlfCount = 0;
		for (size_t start = 0;;)
		{
			const size_t nl = source.find('\n', start);
			std::string_view line = source.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
			const bool cr = nl != std::string_view::npos && !line.empty() && line.back() == '\r';
			if (cr)
			{
				line.remove_suffix(1);
				crlfCount++;
			}
			lines.push_back(line);
			crlf.push_back(cr ? 1 : 0);
			if (nl == std::string_view::npos)
				break;
			start = nl + 1;
		}
		const bool mostlyCrlf = crlfCount * 2 > lines.size() - 1;
		if (source.find('\n') == std::string_view::npos || source.back() != '\n')
			crlf.back() = mostlyCrlf ? 1 : 0;

		// Each line's tokens, and whether a token spanning lines covers its
		// start (left as written) or its end (its trailing text is the token's).
		const size_t n = lines.size();
		std::vector<std::vector<const Token*>> onLine(n);
		std::vector<uint8_t> insideAtStart(n, 0), insideAtEnd(n, 0);
		for (const Token& t : tokens)
		{
			if (t.Kind == TokenKind::Eof)
				break;
			if (t.Line >= 0 && (size_t)t.Line < n)
				onLine[t.Line].push_back(&t);
			for (int l = t.Line; l < t.EndLine && (size_t)l < n; l++)
			{
				insideAtEnd[l] = 1;
				if ((size_t)l + 1 < n)
					insideAtStart[l + 1] = 1;
			}
		}

		// The `>` tokens that close a generic's `<` (`Array<number>`): a
		// line ending in one is finished, not waiting for a comparison's
		// right side. A generic's `<` follows its name with no space; a `>`
		// closes the latest one at its bracket depth.
		std::vector<uint8_t> genericClose(tokens.size(), 0);
		{
			std::vector<std::pair<size_t, int>> angles; // an open `<`'s token and bracket depth
			int depth = 0;
			const Token* prev = nullptr;
			for (size_t k = 0; k < tokens.size(); k++)
			{
				const Token& t = tokens[k];
				if (t.IsComment())
					continue;
				if (IsOpener(t))
					depth++;
				else if (IsCloser(t))
				{
					depth--;
					while (!angles.empty() && angles.back().second > depth)
						angles.pop_back();
				}
				else if (t.Is("<") && prev && prev->Kind == TokenKind::Name && prev->End == t.Begin)
					angles.push_back({ k, depth });
				else if (t.Is(">") && !angles.empty() && angles.back().second == depth)
				{
					angles.pop_back();
					genericClose[k] = 1;
				}
				prev = &t;
			}
		}
		auto awaitsOperand = [&](const Token* t) { return t && AwaitsOperand(*t) && !genericClose[(size_t)(t - tokens.data())]; };

		struct Open
		{
			int LineLevel = 0; // the level of the line it opened on
			size_t Line = 0;
		};
		std::vector<Open> stack;
		const Token* previousLast = nullptr; // the last code token of the line before
		size_t previousLine = 0;
		std::vector<std::string> out;
		out.reserve(n);
		for (size_t l = 0; l < n; l++)
		{
			const std::string_view text = lines[l];
			const std::vector<const Token*>& toks = onLine[l];
			if (insideAtStart[l])
			{
				// Inside a long string or comment: as written, but for what
				// follows where it ends on this line.
				std::string_view kept = text;
				if (!insideAtEnd[l])
					while (!kept.empty() && (kept.back() == ' ' || kept.back() == '\t'))
						kept.remove_suffix(1);
				out.emplace_back(kept);
			}
			else
			{
				const size_t first = text.find_first_not_of(" \t");
				if (first == std::string_view::npos)
					out.emplace_back();
				else
				{
					const Token* lead = toks.empty() ? nullptr : toks.front();
					int level = stack.empty() ? 0 : stack.back().LineLevel + 1;
					if (lead && IsCloser(*lead) && !stack.empty())
						level = stack.back().LineLevel;
					// A continued statement: one level more, unless a bracket the
					// line before opened already gives it one.
					else if (lead && !lead->IsComment() && !(!stack.empty() && stack.back().Line == previousLine && previousLast) &&
						(awaitsOperand(previousLast) || ContinuesLine(*lead) ||
							(lead->Is("else") && previousLast && !previousLast->Is("}"))))
						level += 1;
					std::string line;
					for (int i = 0; i < level; i++)
						line += options.Indent;
					std::string_view body = text.substr(first);
					if (!insideAtEnd[l])
						while (!body.empty() && (body.back() == ' ' || body.back() == '\t'))
							body.remove_suffix(1);
					line += body;
					out.push_back(std::move(line));
				}
			}
			// The brackets this line opens and closes, at this line's level.
			int lineLevel = 0;
			{
				const std::string& done = out.back();
				size_t i = 0;
				while (i < done.size() && done.compare(i, options.Indent.size(), options.Indent) == 0 && !options.Indent.empty())
				{
					i += options.Indent.size();
					lineLevel++;
				}
			}
			for (const Token* t : toks)
			{
				if (t->IsComment())
					continue;
				if (IsOpener(*t))
					stack.push_back({ lineLevel, l });
				else if (IsCloser(*t) && !stack.empty())
					stack.pop_back();
				previousLast = t;
				previousLine = l;
			}
		}

		// No blank lines after the last, one line break at the end.
		while (!out.empty() && out.back().empty())
			out.pop_back();
		for (size_t l = 0; l < out.size(); l++)
		{
			result.Text += out[l];
			result.Text += crlf[l] ? "\r\n" : "\n";
		}
		result.Changed = result.Text != source;
		return result;
	}

} // namespace cluau
