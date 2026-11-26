#include "cluau/convert.h"

#include <algorithm>
#include <cctype>

namespace cluau {

	namespace {

		// A standard-Luau lexer (the translator's lexes cluau, where `--` is
		// an operator and `//` a comment).
		enum class Kind { Name, Number, String, Comment, Punct };

		struct Tok
		{
			Kind K = Kind::Punct;
			size_t Begin = 0;
			size_t End = 0;
			int Line = 0;
			int EndLine = 0; // the line its last character is on (a long string's or comment's can be a later one)
			int Col = 0;
			std::string_view Text;
			bool Is(std::string_view s) const { return (K == Kind::Punct || K == Kind::Name) && Text == s; }
		};

		bool NameStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
		bool NameChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
		// Luau's whitespace (form feed and vertical tab included).
		bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

		// `[`, `=`*, `[` at i: the level (number of '='), or -1.
		int LongBracket(std::string_view s, size_t i)
		{
			if (i >= s.size() || s[i] != '[')
				return -1;
			size_t j = i + 1;
			while (j < s.size() && s[j] == '=')
				j++;
			return j < s.size() && s[j] == '[' ? (int)(j - i - 1) : -1;
		}

		// The end (one past) of a long bracket body opened at i with `level`.
		size_t LongEnd(std::string_view s, size_t i, int level)
		{
			const std::string close = "]" + std::string(level, '=') + "]";
			const size_t at = s.find(close, i + level + 2);
			return at == std::string_view::npos ? s.size() : at + close.size();
		}

		// The end of the escape sequence whose backslash is at i, as Luau
		// reads it: a backslash before a CRLF line break continues the string
		// past both characters, and `\z` skips the whitespace after it,
		// newlines included.
		size_t EscapeEnd(std::string_view s, size_t i)
		{
			size_t j = i + 1;
			if (j >= s.size())
				return j;
			if (s[j] == '\r')
				return j + 1 < s.size() && s[j + 1] == '\n' ? j + 2 : j + 1;
			if (s[j] == 'z')
			{
				j++;
				while (j < s.size() && IsSpace(s[j]))
					j++;
				return j;
			}
			return j + 1;
		}

		// The end (one past the closing quote) of the quoted string at i. An
		// unfinished one ends before its line break.
		size_t QuotedEnd(std::string_view s, size_t i)
		{
			const char quote = s[i];
			size_t j = i + 1;
			while (j < s.size() && s[j] != quote)
			{
				if (s[j] == '\n' || s[j] == '\r')
					return j;
				j = s[j] == '\\' ? EscapeEnd(s, j) : j + 1;
			}
			return std::min(j + 1, s.size());
		}

		// The end of the comment (`--` or `--[[ ]]`) at i.
		size_t CommentEnd(std::string_view s, size_t i)
		{
			if (const int level = LongBracket(s, i + 2); level >= 0)
				return LongEnd(s, i + 2, level);
			const size_t end = s.find('\n', i);
			return end == std::string_view::npos ? s.size() : end;
		}

		// How deeply interpolated strings may nest in one another (the
		// translator's limit; deeper ones are left as written).
		constexpr int kMaxNesting = 200;
		constexpr size_t kNone = (size_t)-1;

		size_t SegmentClose(std::string_view s, size_t k, int nesting);

		// The end (one past the closing backtick) of the interpolated string
		// at i: its `{}` segments are Luau expressions, with their own
		// strings, comments, braces and interpolated strings.
		size_t InterpolatedEnd(std::string_view s, size_t i, int nesting = 0)
		{
			size_t j = i + 1;
			while (j < s.size())
			{
				const char c = s[j];
				if (c == '`')
					return j + 1;
				if (c == '\n' || c == '\r')
					return j; // unfinished
				if (c == '\\')
					j = s.substr(j + 1, 2) == "u{" ? j + 3 : EscapeEnd(s, j);
				else if (c == '{')
				{
					const size_t close = SegmentClose(s, j + 1, nesting);
					if (close == kNone)
						return s.size();
					j = close + 1;
				}
				else
					j++;
			}
			return s.size();
		}

		// The `}` closing the segment whose expression starts at k, or kNone.
		size_t SegmentClose(std::string_view s, size_t k, int nesting)
		{
			int depth = 1;
			while (k < s.size())
			{
				const char c = s[k];
				if (c == '"' || c == '\'')
					k = QuotedEnd(s, k);
				else if (c == '`')
					k = nesting < kMaxNesting ? InterpolatedEnd(s, k, nesting + 1) : k + 1;
				else if (c == '-' && k + 1 < s.size() && s[k + 1] == '-')
					k = CommentEnd(s, k);
				else if (const int level = LongBracket(s, k); level >= 0)
					k = LongEnd(s, k, level);
				else
				{
					if (c == '{')
						depth++;
					else if (c == '}' && --depth == 0)
						return k;
					k++;
				}
			}
			return kNone;
		}

		// The `{expr}` segments of the interpolated string `text`: each
		// expression's offset and length.
		std::vector<std::pair<size_t, size_t>> Segments(std::string_view text, int nesting)
		{
			std::vector<std::pair<size_t, size_t>> out;
			size_t j = 1;
			while (j < text.size() && text[j] != '`')
			{
				if (text[j] == '\\')
					j = text.substr(j + 1, 2) == "u{" ? j + 3 : EscapeEnd(text, j);
				else if (text[j] == '{')
				{
					const size_t close = SegmentClose(text, j + 1, nesting);
					if (close == kNone)
						break;
					out.push_back({ j + 1, close - (j + 1) });
					j = close + 1;
				}
				else
					j++;
			}
			return out;
		}

		// `odd`: the offsets of whitespace cluau does not read as such (form
		// feeds and vertical tabs between tokens).
		std::vector<Tok> Lex(std::string_view s, std::vector<size_t>* odd = nullptr)
		{
			static constexpr std::string_view kPunct3[] = { "...", "..=", "//=" };
			static constexpr std::string_view kPunct2[] = { "..", "==", "~=", "<=", ">=", "//", "->", "::", "+=", "-=", "*=", "/=", "%=", "^=" };
			std::vector<Tok> toks;
			int line = 0;
			size_t lineStart = 0;
			size_t i = 0;
			auto advanceLines = [&](size_t from, size_t to) {
				for (size_t k = from; k < to; k++)
					if (s[k] == '\n')
					{
						line++;
						lineStart = k + 1;
					}
			};
			while (i < s.size())
			{
				const char c = s[i];
				if (IsSpace(c))
				{
					if (odd && (c == '\f' || c == '\v'))
						odd->push_back(i);
					advanceLines(i, i + 1);
					i++;
					continue;
				}
				Tok t;
				t.Begin = i;
				t.Line = line;
				t.Col = (int)(i - lineStart);
				size_t end = i + 1;
				if (c == '-' && i + 1 < s.size() && s[i + 1] == '-')
				{
					t.K = Kind::Comment;
					end = CommentEnd(s, i);
				}
				else if (const int level = LongBracket(s, i); level >= 0)
				{
					t.K = Kind::String;
					end = LongEnd(s, i, level);
				}
				else if (c == '`')
				{
					t.K = Kind::String;
					end = InterpolatedEnd(s, i);
				}
				else if (c == '"' || c == '\'')
				{
					t.K = Kind::String;
					end = QuotedEnd(s, i);
				}
				else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])))
				{
					t.K = Kind::Number;
					while (end < s.size() && (NameChar(s[end]) || (s[end] == '.' && (end + 1 >= s.size() || s[end + 1] != '.'))))
					{
						const char e = (char)std::tolower((unsigned char)s[end]);
						const bool hex = s.substr(i, 2) == "0x" || s.substr(i, 2) == "0X";
						end++;
						if (((e == 'e' && !hex) || e == 'p') && end < s.size() && (s[end] == '+' || s[end] == '-'))
							end++;
					}
				}
				else if (NameStart(c))
				{
					t.K = Kind::Name;
					while (end < s.size() && NameChar(s[end]))
						end++;
				}
				else
				{
					t.K = Kind::Punct;
					for (std::string_view p : kPunct3)
						if (s.substr(i, 3) == p)
							end = i + 3;
					if (end == i + 1)
						for (std::string_view p : kPunct2)
							if (s.substr(i, 2) == p)
								end = i + 2;
				}
				t.End = end;
				t.Text = s.substr(i, end - i);
				advanceLines(i, end);
				t.EndLine = line;
				toks.push_back(t);
				i = end;
			}
			return toks;
		}

		struct Edit
		{
			size_t Begin = 0;
			size_t End = 0;
			std::string Text;
		};

		enum class Block { If, IfExpr, Loop, Function, Do, Repeat, Until };

		struct Frame
		{
			Block B = Block::If;
			bool Awaiting = false; // an if/elseif waiting for `then`, a loop for `do`
			size_t CondStart = 0;  // the condition's first significant token
			size_t Open = 0;       // the token that opened it (`until` for an until condition)
			int Depth = 0;         // brackets opened while this is the innermost frame
			bool DeferredNot = false; // an until condition's leading `not (`, converted when the condition ends
		};

		// The Luau hot comments a file's first lines can hold (`--!strict`).
		bool IsModeComment(std::string_view text)
		{
			size_t end = 3;
			while (end < text.size() && NameChar(text[end]))
				end++;
			const std::string_view word = text.substr(3, end - 3);
			static constexpr std::string_view kModes[] = { "strict", "nonstrict", "nocheck", "native", "optimize", "nolint" };
			return std::find(std::begin(kModes), std::end(kModes), word) != std::end(kModes);
		}

		class Converter
		{
		public:
			// `expression`: the source is one expression (an interpolated
			// string's segment), not a chunk.
			explicit Converter(std::string_view src, bool expression = false, int nesting = 0)
				: m_Src(src), m_Expression(expression), m_Nesting(nesting)
			{
				for (const Tok& t : Lex(src, &m_OddSpaces))
					(t.K == Kind::Comment ? m_Comments : m_Toks).push_back(t);
				m_Match.assign(m_Toks.size(), npos);
				m_Enclosing.assign(m_Toks.size(), npos);
				std::vector<size_t> open;
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Tok& t = m_Toks[i];
					if (!open.empty())
						m_Enclosing[i] = open.back();
					if (t.Is("(") || t.Is("[") || t.Is("{"))
						open.push_back(i);
					else if ((t.Is(")") || t.Is("]") || t.Is("}")) && !open.empty())
					{
						m_Match[open.back()] = i;
						m_Match[i] = open.back();
						open.pop_back();
					}
				}
				m_BodyStart.assign(m_Toks.size(), false);
				MarkTypeEnds();
			}

			ConvertResult Run()
			{
				for (const Tok& c : m_Comments)
					Comment(c);
				for (size_t i = 0; i < m_Toks.size(); i++)
					Token(i);
				while (!m_Stack.empty() && m_Stack.back().B == Block::Until)
				{
					const Frame f = m_Stack.back();
					m_Stack.pop_back();
					FinishUntil(f, m_Toks.size() - 1);
				}
				if (!m_Stack.empty())
					Error(m_Toks.back(), "a block is not closed (a missing 'end')");
				// cluau reads only spaces, tabs and line breaks as whitespace.
				// (Added last: an edit already covering the character wins.)
				for (size_t at : m_OddSpaces)
					m_Edits.push_back({ at, at + 1, " " });
				ConvertResult r;
				std::stable_sort(m_Edits.begin(), m_Edits.end(), [](const Edit& a, const Edit& b) {
					if (a.Begin != b.Begin)
						return a.Begin < b.Begin;
					return (a.Begin == a.End) && (b.Begin != b.End); // insertions first
				});
				size_t at = 0;
				for (const Edit& e : m_Edits)
				{
					if (e.Begin < at)
						continue; // overlapping (a span already rewritten)
					r.Cluau.append(m_Src.substr(at, e.Begin - at));
					r.Cluau += e.Text;
					at = e.End;
				}
				r.Cluau.append(m_Src.substr(at));
				r.Diagnostics = std::move(m_Diagnostics);
				return r;
			}

			// The segment has what cluau cannot hold in an interpolated
			// string: it stays as written.
			bool LeaveAsWritten() const { return m_LeaveAsWritten; }

		private:
			static constexpr size_t npos = (size_t)-1;

			void Replace(const Tok& t, std::string text) { m_Edits.push_back({ t.Begin, t.End, std::move(text) }); }
			void Insert(size_t at, std::string text) { m_Edits.push_back({ at, at, std::move(text) }); }

			void Error(const Tok& t, std::string message, bool warning = false)
			{
				Diagnostic d;
				d.At = { { t.Line, t.Col }, { t.Line, t.Col + (int)t.Text.size() } };
				d.Message = std::move(message);
				d.Warning = warning;
				m_Diagnostics.push_back(std::move(d));
			}

			const Tok& At(size_t i) const
			{
				static const Tok kNoToken;
				return i < m_Toks.size() ? m_Toks[i] : kNoToken;
			}

			// The first (or, with `last`, the last) comment between the
			// offsets `from` and `to`, or null.
			const Tok* CommentBetween(size_t from, size_t to, bool last = false) const
			{
				auto begins = [](const Tok& c, size_t at) { return c.Begin < at; };
				auto it = std::lower_bound(m_Comments.begin(), m_Comments.end(), from, begins);
				if (it == m_Comments.end() || it->Begin >= to)
					return nullptr;
				if (!last)
					return &*it;
				auto end = std::lower_bound(it, m_Comments.end(), to, begins);
				return &*(end - 1);
			}

			// Only whitespace precedes offset `at` on its line.
			bool FirstOnLine(size_t at) const
			{
				while (at > 0 && m_Src[at - 1] != '\n' && IsSpace(m_Src[at - 1]))
					at--;
				return at == 0 || m_Src[at - 1] == '\n';
			}

			void Comment(const Tok& c)
			{
				const std::string_view text = c.Text;
				if (m_Expression)
				{
					// The translator copies an interpolated string's segments
					// as they are, so a comment in one would reach Luau as
					// cluau's: it goes (it holds no code).
					Replace(c, " ");
					Error(c, "a comment inside an interpolated string's braces was dropped", true);
					return;
				}
				// After a `/` (`x /--[[c]] 2`), `/*` or `//` would read as `//`.
				const std::string lead = c.Begin > 0 && m_Src[c.Begin - 1] == '/' ? " " : "";
				if (const int level = LongBracket(text, 2); level >= 0)
				{
					const size_t open = 2 + level + 2;
					const size_t close = text.size() >= open + level + 2 ? text.size() - (level + 2) : text.size();
					std::string body(text.substr(open, close > open ? close - open : 0));
					for (size_t p = body.find("*/"); p != std::string::npos; p = body.find("*/", p + 3))
						body.replace(p, 2, "* /");
					Replace(c, lead + "/*" + body + "*/");
					return;
				}
				if (text.size() > 2 && text[2] == '!')
				{
					// A mode comment before the code is dropped (the host
					// decides the mode); cluau refuses `//!` at the start of
					// any line, so another one there keeps its text as `// !`.
					if ((m_Toks.empty() || c.Begin < m_Toks.front().Begin) && IsModeComment(text))
					{
						Replace(c, "");
						Error(c, "the mode comment was dropped: cluau hosts decide the mode", true);
						return;
					}
					if (FirstOnLine(c.Begin))
					{
						Replace(c, "// " + std::string(text.substr(2)));
						return;
					}
				}
				size_t dashes = 0;
				while (dashes < text.size() && text[dashes] == '-')
					dashes++;
				Replace(c, lead + std::string(dashes, '/') + std::string(text.substr(dashes)));
			}

			// The previous significant token makes the next an expression.
			bool ExpressionContext(size_t i) const
			{
				if (i == 0)
					return m_Expression;
				const Tok& p = m_Toks[i - 1];
				if (m_TypeEnd[i - 1] || m_BodyStart[i - 1])
					return false;
				if (p.Is(";"))
				{
					// In a table constructor (`{ 1; if a then 2 else 3 }`) a
					// `;` separates fields: a `{` opened inside the innermost
					// block is a table (Luau's blocks have no braces).
					const size_t b = m_Enclosing[i];
					return b != npos && At(b).Is("{") && (m_Stack.empty() || m_Stack.back().Open < b);
				}
				if (p.K == Kind::Punct)
					return !(p.Is(")") || p.Is("]") || p.Is("}") || p.Is("...") || p.Is("?"));
				if (p.K == Kind::Name)
				{
					static constexpr std::string_view kExpr[] = { "return", "and", "or", "not", "in", "until", "if", "elseif", "while" };
					for (std::string_view w : kExpr)
						if (p.Text == w)
							return true;
					if ((p.Is("then") || p.Is("else") || p.Is("elseif")) && m_LastIfExprWord == i - 1)
						return true;
				}
				return false;
			}

			// The ends of the types a statement can end with: a declaration's
			// annotation, a type alias, a cast. (`local best: string?` then
			// `if` is a statement, not an if-expression after an operator.)
			void MarkTypeEnds()
			{
				m_TypeEnd.assign(m_Toks.size(), false);
				auto mark = [&](size_t start) {
					const size_t end = SkipType(start);
					if (end != npos && end < m_Toks.size())
						m_TypeEnd[end] = true;
					return end;
				};
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Tok& t = m_Toks[i];
					const bool member = i > 0 && (At(i - 1).Is(".") || At(i - 1).Is(":"));
					// Luau's `const x: T = ...` declares like `local`.
					const bool constDeclaration =
						t.Is("const") && !member && At(i + 1).K == Kind::Name && (At(i + 2).Is(":") || At(i + 2).Is("=") || At(i + 2).Is(","));
					if (t.Is("::"))
						mark(i + 1);
					else if (t.Is("type") && !member && At(i + 1).K == Kind::Name && (At(i + 2).Is("=") || At(i + 2).Is("<")))
					{
						size_t k = i + 2;
						if (At(k).Is("<"))
							k = SkipAngles(k) + 1;
						if (At(k).Is("="))
							mark(k + 1);
					}
					else if ((t.Is("local") && !At(i + 1).Is("function")) || constDeclaration)
					{
						size_t j = i + 1;
						while (At(j).K == Kind::Name)
						{
							j++;
							if (At(j).Is(":"))
							{
								const size_t end = mark(j + 1);
								if (end == npos)
									break;
								j = end + 1;
							}
							if (!At(j).Is(","))
								break;
							j++;
						}
					}
				}
			}

			// `continue` is no keyword here: Luau reads it as a name unless it
			// stands alone as a statement.
			static bool IsKeyword(std::string_view w)
			{
				static constexpr std::string_view kWords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
					"local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };
				return std::find(std::begin(kWords), std::end(kWords), w) != std::end(kWords);
			}

			// Token i can be an expression's last token (a value, a closing
			// bracket, a type a declaration ends with).
			bool EndsOperand(size_t i) const
			{
				const Tok& t = At(i);
				if (t.K == Kind::Number || t.K == Kind::String || (i < m_TypeEnd.size() && m_TypeEnd[i]))
					return true;
				if (t.K == Kind::Name)
					return !IsKeyword(t.Text) || t.Is("end") || t.Is("true") || t.Is("false") || t.Is("nil");
				return t.Is(")") || t.Is("]") || t.Is("}") || t.Is("...");
			}

			// A statement can start at token i (in Luau).
			bool StatementStart(size_t i) const
			{
				if (i == 0)
					return !m_Expression;
				const Tok& p = At(i - 1);
				return p.Is(";") || p.Is("then") || p.Is("do") || p.Is("else") || p.Is("repeat") || p.Is("break") || p.Is("continue") ||
					m_BodyStart[i - 1] || EndsOperand(i - 1);
			}

			// cluau reads a statement start at token i (the translator's
			// rule): on the line where the previous token ends, only after a
			// `;` or a block's brace.
			bool CluauStatementStart(size_t i) const
			{
				if (!StatementStart(i) || i == 0)
					return i == 0 && !m_Expression;
				const Tok& p = At(i - 1);
				if (p.EndLine != At(i).Line)
					return true;
				return p.Is(";") || p.Is("do") || p.Is("repeat") || p.Is("end") || m_BodyStart[i - 1] ||
					((p.Is("then") || p.Is("else")) && m_LastIfExprWord != i - 1);
			}

			// The `{expr}` segments of an interpolated string convert as
			// expressions (their operators and if-expressions take cluau's
			// spelling, which the translator reads back).
			void Interpolated(const Tok& t)
			{
				if (m_Nesting >= kMaxNesting)
					return;
				std::string out;
				size_t at = 0;
				bool changed = false;
				for (const auto& [offset, length] : Segments(t.Text, m_Nesting))
				{
					const std::string_view segment = t.Text.substr(offset, length);
					Converter inner(segment, true, m_Nesting + 1);
					ConvertResult r = inner.Run();
					for (const Diagnostic& d : r.Diagnostics)
						Error(t, d.Message, d.Warning);
					if (inner.LeaveAsWritten())
						r.Cluau = std::string(segment);
					out.append(t.Text.substr(at, offset - at));
					out += r.Cluau;
					changed = changed || r.Cluau != segment;
					at = offset + length;
				}
				if (!changed)
					return;
				out.append(t.Text.substr(at));
				Replace(t, out);
			}

			// The condition between `CondStart` and `closer` gets parentheses
			// unless it already is one parenthesised group.
			void Parenthesise(size_t condStart, size_t closer)
			{
				if (condStart >= closer)
					return;
				if (At(condStart).Is("(") && m_Match[condStart] == closer - 1)
					return;
				Insert(At(condStart).Begin, "(");
				Insert(At(closer - 1).End, ")");
			}

			// `not` -> `!` (with the space after it, up to a comment).
			void Not(size_t i)
			{
				const Tok& t = m_Toks[i];
				const Tok& next = At(i + 1);
				if (next.Line == t.Line && next.Begin > t.End)
				{
					const Tok* comment = CommentBetween(t.End, next.Begin);
					m_Edits.push_back({ t.Begin, comment ? comment->Begin : next.Begin, "!" });
				}
				else
					Replace(t, "!");
			}

			void Token(size_t i)
			{
				EndUntilConditions(i);
				const Tok& t = m_Toks[i];
				if (t.K == Kind::Punct)
				{
					Frame* top = m_Stack.empty() ? nullptr : &m_Stack.back();
					if (top && (t.Is("(") || t.Is("[") || t.Is("{")))
						top->Depth++;
					else if (top && top->Depth > 0 && (t.Is(")") || t.Is("]") || t.Is("}")))
						top->Depth--;
					if (t.Is("~="))
						Replace(t, "!=");
					else if (t.Is("//"))
						Replace(t, "~/");
					else if (t.Is("//="))
						Replace(t, "~/=");
					else if (t.Is("{") && i > 0 && At(i - 1).Line != t.Line && m_Match[i] != npos && EndsOperand(i - 1) && !m_TypeEnd[i - 1] &&
						!At(i - 1).Is("..."))
					{
						// `f` and `{ ... }` on the next line is Luau's call; in cluau
						// a `{` starting a line after a finished statement is a
						// block, so the call gets its parentheses (the `(` on the
						// callee's line: one starting a line would be ambiguous).
						// cluau counts the lines from where that token starts
						// (`f "a\z` then `b" { ... }`).
						Insert(At(i - 1).End, "(");
						Insert(At(m_Match[i]).End, ")");
					}
					return;
				}
				if (t.K == Kind::String && t.Text.front() == '`')
				{
					Interpolated(t);
					return;
				}
				if (t.K == Kind::String && m_Expression)
				{
					// The translator finds a segment's end by its braces, quotes
					// and backticks, which a long string's text would confuse.
					if (const int level = LongBracket(t.Text, 0); level >= 0 && t.Text.find_first_of("{}`\"'", level + 2) != std::string_view::npos)
					{
						Error(t, "a long string holding braces, quotes or backticks inside an interpolated string's braces has no cluau form (left as written)");
						m_LeaveAsWritten = true;
					}
					return;
				}
				if (t.K != Kind::Name)
					return;
				if (i > 0 && (At(i - 1).Is(".") || At(i - 1).Is(":")) && !At(i - 1).Is("::"))
					return; // a member name (`t.end` is not valid Luau, but `a.if` reads safely)
				const std::string_view w = t.Text;
				Frame* top = m_Stack.empty() ? nullptr : &m_Stack.back();
				if (w == "if")
				{
					if (ExpressionContext(i))
					{
						// A table's `;` before an if-expression is written `,`
						// (the same separator), where cluau reads no statement.
						if (At(i - 1).Is(";"))
							Replace(At(i - 1), ",");
						m_Stack.push_back({ Block::IfExpr, true, i + 1, i });
					}
					else
						m_Stack.push_back({ Block::If, true, i + 1, i });
				}
				else if (w == "elseif")
				{
					if (top && top->B == Block::IfExpr)
					{
						Replace(t, "else if");
						top->Awaiting = true;
						top->CondStart = i + 1;
						m_LastIfExprWord = i;
					}
					else if (top && top->B == Block::If)
					{
						Replace(t, "} else if");
						top->Awaiting = true;
						top->CondStart = i + 1;
					}
					else
						Error(t, "'elseif' outside an if");
				}
				else if (w == "then")
				{
					if (top && top->B == Block::IfExpr && top->Awaiting)
					{
						// `if (c) a`: the condition parenthesised, `then` gone
						// (with the space before it when on the same line, up
						// to a comment there).
						Parenthesise(top->CondStart, i);
						const Tok& prev = At(i - 1);
						if (i > 0 && prev.EndLine == t.Line)
						{
							const Tok* comment = CommentBetween(prev.End, t.Begin, true);
							m_Edits.push_back({ comment ? comment->End : prev.End, t.End, "" });
						}
						else
							Replace(t, "");
						top->Awaiting = false;
						m_LastIfExprWord = i;
					}
					else if (top && top->B == Block::If && top->Awaiting)
					{
						Parenthesise(top->CondStart, i);
						Replace(t, "{");
						top->Awaiting = false;
					}
					else
						Error(t, "'then' without an if");
				}
				else if (w == "else")
				{
					if (top && top->B == Block::IfExpr)
					{
						m_LastIfExprWord = i;
						m_Stack.pop_back();
					}
					else if (top && top->B == Block::If)
						Replace(t, "} else {");
					else
						Error(t, "'else' outside an if");
				}
				else if (w == "while" || w == "for")
					m_Stack.push_back({ Block::Loop, true, i + 1, i });
				else if (w == "do")
				{
					if (top && top->B == Block::Loop && top->Awaiting)
					{
						Parenthesise(top->CondStart, i);
						top->Awaiting = false;
					}
					else
					{
						m_Stack.push_back({ Block::Do, false, 0, i });
						if (i > 0 && DoBlockNeedsSemicolon(i))
						{
							Replace(t, "; {");
							return;
						}
					}
					Replace(t, "{");
				}
				else if (w == "repeat")
				{
					m_Stack.push_back({ Block::Repeat, false, 0, i });
					Replace(t, "do {");
				}
				else if (w == "until")
				{
					if (!top || top->B != Block::Repeat)
						Error(t, "'until' without a repeat");
					else
					{
						m_Stack.pop_back();
						m_Stack.push_back({ Block::Until, false, i + 1, i });
					}
				}
				else if (w == "function")
					Function(i);
				else if (w == "end")
				{
					while (top && top->B == Block::IfExpr)
					{
						m_Stack.pop_back();
						top = m_Stack.empty() ? nullptr : &m_Stack.back();
					}
					if (!top || top->B == Block::Repeat || top->B == Block::Until || top->Awaiting)
						Error(t, "'end' does not close a block");
					else
						m_Stack.pop_back();
					Replace(t, "}");
				}
				else if (w == "local")
				{
					if (i > 0 && (At(i - 1).Is("if") || At(i - 1).Is("elseif") || At(i - 1).Is("while")))
						Error(t, "'" + std::string(At(i - 1).Text) + " local' has no cluau form: declare the local before it (left as written)");
					else if (!At(i + 1).Is("function"))
						Replace(t, "let");
				}
				else if (w == "and")
					Replace(t, "&&");
				else if (w == "or")
					Replace(t, "||");
				else if (w == "not")
				{
					// `until not (c)` may become `while (c)`: decided when the
					// condition ends.
					if (top && top->B == Block::Until && top->Open == i - 1 && At(i + 1).Is("(") && At(i + 1).Line == At(i - 1).Line)
						top->DeferredNot = true;
					else
						Not(i);
				}
				else if (w == "const")
				{
					// Luau's `const x = 1` and `const function f()` are cluau's
					// too; `const` as a name is not.
					const bool declaration = StatementStart(i) && (At(i + 1).Is("function") || (At(i + 1).K == Kind::Name && !IsKeyword(At(i + 1).Text)));
					if (!declaration)
						Error(t, "'const' is a cluau keyword: rename it (left as written)");
				}
				else if (w == "let" || w == "class")
					Error(t, "'" + std::string(w) + "' is a cluau keyword: rename it (left as written)");
				else if ((w == "throw" || w == "try") && CluauStatementStart(i) && At(i + 1).Line == t.Line &&
					(At(i + 1).Is("{") || (w == "throw" && (At(i + 1).Is("(") || At(i + 1).K == Kind::String))))
					Error(t, "a call to '" + std::string(w) + "' here reads as cluau's " + std::string(w) +
						" statement: rename the function (left as written)");
				else if (w == "switch" && CluauStatementStart(i) && At(i + 1).Is("(") && m_Match[i + 1] != npos &&
					At(m_Match[i + 1] + 1).Is("{"))
					Error(t, "a call to 'switch' here reads as cluau's switch statement: rename the function (left as written)");
			}

			// A do-block at token i is written `; {`, so that cluau does not
			// read its `{` as part of what comes before: call sugar after a
			// value on its line (`local f do ... end`), a type it ends
			// (`local x: number? do`), or a name cluau reads as an expression
			// word, whose `{` is a table even on the next line
			// (`local v = export` then `do`).
			bool DoBlockNeedsSemicolon(size_t i) const
			{
				if (m_UntilEnd == i - 1)
					return false; // the do-while's own `;` ends it
				const Tok& prev = At(i - 1);
				const bool member = i > 1 && (At(i - 2).Is(".") || At(i - 2).Is(":")) && !At(i - 2).Is("::");
				if ((prev.Is("type") || prev.Is("export")) && !member)
					return true;
				if (prev.EndLine != At(i).Line || m_BodyStart[i - 1])
					return false;
				if (m_TypeEnd[i - 1])
					return true;
				return (prev.K != Kind::Punct || prev.Is(")") || prev.Is("]") || prev.Is("}") || prev.Is("...")) &&
					!(prev.Is("then") || prev.Is("else") || prev.Is("do") || prev.Is("repeat"));
			}

			// `function [name][<generics>](params)[: type]` gets ` {` after its
			// signature.
			void Function(size_t i)
			{
				m_Stack.push_back({ Block::Function, false, 0, i });
				if (m_Expression)
				{
					// The translator copies an interpolated string's segments
					// as they are, so a function's cluau braces would reach
					// Luau.
					Error(At(i), "a function inside an interpolated string's braces has no cluau form: make it a local (left as written)");
					m_LeaveAsWritten = true;
				}
				size_t j = i + 1;
				if (At(j).K == Kind::Name && !At(j).Is("("))
				{
					j++;
					while ((At(j).Is(".") || At(j).Is(":")) && At(j + 1).K == Kind::Name)
						j += 2;
				}
				if (At(j).Is("<"))
					j = SkipAngles(j) + 1;
				if (!At(j).Is("(") || m_Match[j] == npos)
				{
					Error(At(i), "a function without a parameter list");
					return;
				}
				size_t last = m_Match[j];
				if (At(last + 1).Is(":"))
				{
					const size_t end = SkipType(last + 2);
					if (end != npos)
						last = end;
				}
				m_BodyStart[last] = true;
				Insert(At(last).End, " {");
			}

			// The index of the `>` closing the `<` at i (or, when none does
			// before a token no type holds, the token before that one).
			size_t SkipAngles(size_t i) const
			{
				int depth = 0;
				for (size_t j = i; j < m_Toks.size(); j++)
				{
					const Tok& t = At(j);
					if ((t.Is("(") || t.Is("[") || t.Is("{")) && m_Match[j] != npos)
						j = m_Match[j]; // `Foo<typeof(a and b)>`: a bracket's contents are no stop
					else if (t.Is("<"))
						depth++;
					else if (t.Is(">") && --depth == 0)
						return j;
					else if (t.Is(";") || (t.K == Kind::Name && IsKeyword(t.Text) && !(t.Is("nil") || t.Is("true") || t.Is("false"))) ||
						((t.Is(")") || t.Is("]") || t.Is("}")) && (m_Match[j] == npos || m_Match[j] < i)))
						return j > i ? j - 1 : i;
				}
				return m_Toks.size() - 1;
			}

			// A type annotation starting at i: the index of its last token. A
			// union, or a chain of function types (`() -> () -> T`), is a loop
			// rather than nested calls, however long it is.
			size_t SkipType(size_t i) const
			{
				for (;;)
				{
					if (At(i).Is("|") || At(i).Is("&"))
						i++;
					if (At(i).Is("<"))
						i = SkipAngles(i) + 1; // a generic function type
					if (At(i).Is("..."))
					{
						i++; // a type pack: `...number`
						continue;
					}
					const size_t last = SkipTypeMember(i);
					if (last == npos)
						return npos;
					if ((At(i).Is("(") && At(last + 1).Is("->")) || At(last + 1).Is("|") || At(last + 1).Is("&"))
					{
						i = last + 2;
						continue;
					}
					return last;
				}
			}

			size_t SkipTypeMember(size_t i) const
			{
				size_t last = npos;
				const Tok& t = At(i);
				if (t.Is("(") && m_Match[i] != npos)
					last = m_Match[i];
				else if (t.Is("{") && m_Match[i] != npos)
					last = m_Match[i];
				else if (t.Is("typeof") && At(i + 1).Is("(") && m_Match[i + 1] != npos)
					last = m_Match[i + 1];
				else if (t.K == Kind::String)
					last = i;
				else if (t.K == Kind::Name && (!IsKeyword(t.Text) || t.Is("nil") || t.Is("true") || t.Is("false")))
				{
					// (`x = type y = if ...` is no type alias: `if` is no type.)
					last = i;
					while (At(last + 1).Is(".") && At(last + 2).K == Kind::Name)
						last += 2;
					if (At(last + 1).Is("<"))
						last = SkipAngles(last + 1);
					if (At(last + 1).Is("..."))
						last++; // a generic type pack: `T...`
				}
				else
					return npos;
				while (At(last + 1).Is("?"))
					last++;
				return last;
			}

			// Token i can end a prefix expression, which an index or a call
			// may follow: a name, `)`, `]`, or a call's string or table
			// argument (`f "x"`, `f { }`), but not a literal (`"x"(1)` is
			// no call).
			bool EndsPrefix(size_t i) const
			{
				for (;;)
				{
					const Tok& t = At(i);
					if (t.K == Kind::Name)
						return !IsKeyword(t.Text);
					if (t.Is(")") || t.Is("]"))
						return true;
					if (t.K == Kind::String && t.Text.front() != '`' && i > 0)
						i--;
					else if (t.Is("}") && m_Match[i] != npos && m_Match[i] > 0)
						i = m_Match[i] - 1;
					else
						return false;
				}
			}

			// An until condition goes on while its tokens can continue an
			// expression: whatever follows an operator (`until a and` then
			// `b` on the next line), and after an operand a binary operator
			// or a cast, or after a prefix expression an index or a call's
			// arguments. Anything else starts the next statement.
			bool ContinuesCondition(size_t i) const
			{
				static constexpr std::string_view kBinary[] = { "+", "-", "*", "/", "//", "%", "^", "..", "==", "~=", "<", "<=", ">", ">=", "and", "or",
					"::", "?", "|", "&", "->" };
				const Tok& k = At(i);
				if (k.Is(";"))
					return false;
				if (!EndsOperand(i - 1))
					return true;
				for (std::string_view op : kBinary)
					if (k.Is(op))
						return true;
				if (!EndsPrefix(i - 1))
					return false;
				if (k.Is("("))
					return At(i - 1).EndLine == k.Line; // on another line Luau reads no call
				return k.K == Kind::String || k.Is(".") || k.Is(":") || k.Is("[") || k.Is("{");
			}

			void EndUntilConditions(size_t i)
			{
				while (!m_Stack.empty() && m_Stack.back().B == Block::Until && m_Stack.back().Depth == 0 &&
					(i > m_Stack.back().Open + 1 ? !ContinuesCondition(i) : At(i).Is(";")))
				{
					const Frame f = m_Stack.back();
					m_Stack.pop_back();
					FinishUntil(f, i - 1);
				}
			}

			// `until c` -> `} while (!(c));`, the condition being tokens
			// `f.Open + 1` to `last`.
			void FinishUntil(const Frame& f, size_t last)
			{
				const Tok& t = m_Toks[f.Open];
				const size_t first = f.Open + 1;
				if (first > last || first >= m_Toks.size())
				{
					Error(t, "'until' without a condition");
					return;
				}
				// The source's own `;` ends the do-while (Luau refuses `;;`).
				const char* semi = At(last + 1).Is(";") ? "" : ";";
				m_UntilEnd = last;
				// `until not (c)` reads best as `while (c)`.
				if (f.DeferredNot)
				{
					if (m_Match[first + 1] == last)
					{
						m_Edits.push_back({ t.Begin, At(first + 1).Begin, "} while " });
						Insert(At(last).End, semi);
						return;
					}
					Not(first);
				}
				const bool grouped = At(first).Is("(") && m_Match[first] == last;
				// Through the space to the condition when it is on this line
				// (a comment there stays).
				size_t to = At(first).Line == t.Line ? At(first).Begin : t.End;
				if (const Tok* comment = CommentBetween(t.End, to))
					to = comment->Begin;
				m_Edits.push_back({ t.Begin, to, grouped ? "} while (!" : "} while (!(" });
				Insert(At(last).End, std::string(grouped ? ")" : "))") + semi);
			}

			std::string_view m_Src;
			bool m_Expression = false;
			int m_Nesting = 0; // interpolated strings around this source
			bool m_LeaveAsWritten = false;
			std::vector<bool> m_TypeEnd;   // the last token of a type a statement can end with (`local x: T?`)
			std::vector<bool> m_BodyStart; // a function signature's last token (its body follows)
			std::vector<Tok> m_Toks;
			std::vector<Tok> m_Comments;
			std::vector<size_t> m_OddSpaces;
			std::vector<size_t> m_Match;
			std::vector<size_t> m_Enclosing; // the innermost bracket open around each token
			std::vector<Frame> m_Stack;
			std::vector<Edit> m_Edits;
			std::vector<Diagnostic> m_Diagnostics;
			size_t m_LastIfExprWord = npos;
			size_t m_UntilEnd = npos; // the last token of the latest until condition
		};

	} // namespace

	bool ConvertResult::HasErrors() const
	{
		return std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const Diagnostic& d) { return !d.Warning; });
	}

	namespace {

		constexpr std::string_view kBom = "\xEF\xBB\xBF";

		// Without a UTF-8 byte order mark (as the runtime reads a file).
		std::string_view WithoutBom(std::string_view text)
		{
			return text.substr(0, kBom.size()) == kBom ? text.substr(kBom.size()) : text;
		}

	} // namespace

	ConvertResult Convert(std::string_view luau)
	{
		const std::string_view body = WithoutBom(luau);
		ConvertResult r = Converter(body).Run();
		if (body.size() != luau.size())
			r.Cluau.insert(0, kBom);
		return r;
	}

	namespace {

		// The tokens a round trip compares: no comments, parentheses or `;`
		// (nor a table's `,` before an if-expression, which the conversion
		// writes for a `;`), and an interpolated string's segments as their
		// own tokens (the translator respells the operators in them). Luau's
		// `const` declaration translates back as `local`.
		void Normalise(std::string_view text, std::vector<Tok>& out, int line = -1, int nesting = 0)
		{
			std::vector<char> open;
			for (Tok t : Lex(text))
			{
				if (line >= 0)
					t.Line = line;
				if (t.Is("(") || t.Is("[") || t.Is("{"))
					open.push_back(t.Text[0]);
				else if ((t.Is(")") || t.Is("]") || t.Is("}")) && !open.empty())
					open.pop_back();
				if (t.K == Kind::Comment || t.Is("(") || t.Is(")") || t.Is(";"))
					continue;
				if (t.K == Kind::String && t.Text.front() == '`' && nesting < kMaxNesting)
				{
					size_t at = 0;
					for (const auto& [offset, length] : Segments(t.Text, nesting))
					{
						Tok literal = t;
						literal.Text = t.Text.substr(at, offset - at);
						out.push_back(literal);
						Normalise(t.Text.substr(offset, length), out, t.Line, nesting + 1);
						at = offset + length;
					}
					Tok literal = t;
					literal.Text = t.Text.substr(at);
					out.push_back(literal);
					continue;
				}
				if (t.Is("not") && !out.empty() && out.back().Is("not"))
				{
					out.pop_back();
					continue;
				}
				// An if-expression's `else if c then` is `elseif c then` (cluau
				// writes both as `else if`).
				if (t.Is("if") && !out.empty() && out.back().Is("else"))
				{
					out.back().Text = "elseif";
					continue;
				}
				if (t.Is("if") && !out.empty() && out.back().Is(",") && !open.empty() && open.back() == '{')
					out.pop_back();
				if (t.Is("const") && !(!out.empty() && (out.back().Is(".") || out.back().Is(":"))))
					t.Text = "local";
				out.push_back(t);
			}
		}

	} // namespace

	std::string VerifyConversion(std::string_view luau, std::string_view converted, std::string* roundTrip)
	{
		luau = WithoutBom(luau);
		const TranslateResult back = Translate(WithoutBom(converted));
		for (const Diagnostic& d : back.Diagnostics)
			if (!d.Warning)
				return "line " + std::to_string(d.At.Begin.Line + 1) + ": the conversion does not translate: " + d.Message;
		// A host's translation takes Luau's `error` on its first line (for
		// throw and try): not part of the round trip.
		std::string_view translated = back.Luau;
		constexpr std::string_view kRaiseHead = "local __cluau_error = error; ";
		if (translated.substr(0, kRaiseHead.size()) == kRaiseHead)
			translated.remove_prefix(kRaiseHead.size());
		if (roundTrip)
			*roundTrip = std::string(translated);
		std::vector<Tok> a, b;
		Normalise(luau, a);
		Normalise(translated, b);
		for (size_t i = 0; i < std::max(a.size(), b.size()); i++)
		{
			const std::string_view x = i < a.size() ? a[i].Text : "<end>";
			const std::string_view y = i < b.size() ? b[i].Text : "<end>";
			if (x != y)
				return "line " + std::to_string((i < a.size() ? a[i].Line : a.empty() ? 0 : a.back().Line) + 1) + ": expected '"
					+ std::string(x) + "', the round trip has '" + std::string(y) + "'";
			// An if-expression's `then` holds no code: the translator puts it
			// back after the condition, where StyLua's layout has it a line
			// later.
			if (i < a.size() && i < b.size() && a[i].Line != b[i].Line && x != "then")
				return "line " + std::to_string(a[i].Line + 1) + ": '" + std::string(x) + "' moved to line " + std::to_string(b[i].Line + 1);
		}
		return {};
	}

} // namespace cluau
