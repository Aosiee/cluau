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
			int Col = 0;
			std::string_view Text;
			bool Is(std::string_view s) const { return (K == Kind::Punct || K == Kind::Name) && Text == s; }
		};

		bool NameStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
		bool NameChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

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

		// How deeply interpolated strings may nest in one another (the
		// translator's limit; deeper ones are left as written).
		constexpr int kMaxNesting = 200;

		// The end (one past the closing backtick) of the interpolated string
		// at i: its `{}` segments may hold strings, braces and other
		// interpolated strings.
		size_t InterpolatedEnd(std::string_view s, size_t i, int nesting = 0)
		{
			size_t j = i + 1;
			int depth = 0;
			while (j < s.size())
			{
				const char c = s[j];
				if (depth == 0)
				{
					if (c == '\\')
						j += 2;
					else if (c == '`')
						return j + 1;
					else
					{
						depth = c == '{' ? 1 : 0;
						j++;
					}
					continue;
				}
				if (c == '"' || c == '\'')
				{
					j++;
					while (j < s.size() && s[j] != c && s[j] != '\n')
						j += s[j] == '\\' ? 2 : 1;
					j++;
					continue;
				}
				if (c == '`' && nesting < kMaxNesting)
				{
					j = InterpolatedEnd(s, j, nesting + 1);
					continue;
				}
				if (c == '{')
					depth++;
				else if (c == '}')
					depth--;
				j++;
			}
			return s.size();
		}

		// The `{expr}` segments of the interpolated string `text`: each
		// expression's offset and length.
		std::vector<std::pair<size_t, size_t>> Segments(std::string_view text)
		{
			std::vector<std::pair<size_t, size_t>> out;
			size_t j = 1;
			while (j + 1 < text.size())
			{
				if (text[j] == '\\')
				{
					j += 2;
					continue;
				}
				if (text[j] != '{')
				{
					j++;
					continue;
				}
				// The segment's closing brace (strings in it skipped).
				size_t k = j + 1;
				int depth = 1;
				while (k < text.size() && depth > 0)
				{
					const char c = text[k];
					if (c == '"' || c == '\'')
					{
						k++;
						while (k < text.size() && text[k] != c && text[k] != '\n')
							k += text[k] == '\\' ? 2 : 1;
					}
					else if (c == '`')
					{
						k = InterpolatedEnd(text, k);
						continue;
					}
					else if (c == '{')
						depth++;
					else if (c == '}')
						depth--;
					k++;
				}
				if (depth != 0)
					break;
				out.push_back({ j + 1, k - 1 - (j + 1) });
				j = k;
			}
			return out;
		}

		std::vector<Tok> Lex(std::string_view s)
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
				if (c == '\n' || std::isspace((unsigned char)c))
				{
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
					if (const int level = LongBracket(s, i + 2); level >= 0)
						end = LongEnd(s, i + 2, level);
					else
					{
						end = s.find('\n', i);
						if (end == std::string_view::npos)
							end = s.size();
					}
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
					while (end < s.size() && s[end] != c && s[end] != '\n')
					{
						if (s[end] != '\\')
						{
							end++;
							continue;
						}
						const bool skipSpace = end + 1 < s.size() && s[end + 1] == 'z'; // `\z` skips whitespace, newlines included
						end += 2;
						while (skipSpace && end < s.size() && std::isspace((unsigned char)s[end]))
							end++;
					}
					end = std::min(end + 1, s.size());
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

		enum class Block { If, IfExpr, Loop, Function, Do, Repeat };

		struct Frame
		{
			Block B = Block::If;
			bool Awaiting = false; // an if/elseif waiting for `then`, a loop for `do`
			size_t CondStart = 0;  // the condition's first significant token
		};

		class Converter
		{
		public:
			// `expression`: the source is one expression (an interpolated
			// string's segment), not a chunk.
			explicit Converter(std::string_view src, bool expression = false, int nesting = 0)
				: m_Src(src), m_Expression(expression), m_Nesting(nesting)
			{
				for (const Tok& t : Lex(src))
					(t.K == Kind::Comment ? m_Comments : m_Toks).push_back(t);
				m_Match.assign(m_Toks.size(), npos);
				std::vector<size_t> open;
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Tok& t = m_Toks[i];
					if (t.Is("(") || t.Is("[") || t.Is("{"))
						open.push_back(i);
					else if ((t.Is(")") || t.Is("]") || t.Is("}")) && !open.empty())
					{
						m_Match[open.back()] = i;
						m_Match[i] = open.back();
						open.pop_back();
					}
				}
				m_Skip.assign(m_Toks.size(), false);
				m_BodyStart.assign(m_Toks.size(), false);
				MarkTypeEnds();
			}

			ConvertResult Run()
			{
				for (const Tok& c : m_Comments)
					Comment(c);
				for (size_t i = 0; i < m_Toks.size(); i++)
					if (!m_Skip[i])
						Token(i);
				if (!m_Stack.empty())
					Error(m_Toks.back(), "a block is not closed (a missing 'end')");
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
						continue; // overlapping (never produced on valid input)
					r.Cluau.append(m_Src.substr(at, e.Begin - at));
					r.Cluau += e.Text;
					at = e.End;
				}
				r.Cluau.append(m_Src.substr(at));
				r.Diagnostics = std::move(m_Diagnostics);
				return r;
			}

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
				static const Tok kNone;
				return i < m_Toks.size() ? m_Toks[i] : kNone;
			}

			void Comment(const Tok& c)
			{
				const std::string_view text = c.Text;
				if (const int level = LongBracket(text, 2); level >= 0)
				{
					const size_t open = 2 + level + 2;
					const size_t close = text.size() >= open + level + 2 ? text.size() - (level + 2) : text.size();
					std::string body(text.substr(open, close > open ? close - open : 0));
					for (size_t p = body.find("*/"); p != std::string::npos; p = body.find("*/", p + 3))
						body.replace(p, 2, "* /");
					Replace(c, "/*" + body + "*/");
					return;
				}
				if (text.size() > 2 && text[2] == '!' && c.Col == 0)
				{
					Replace(c, "");
					Error(c, "the mode comment was dropped: cluau hosts decide the mode", true);
					return;
				}
				size_t dashes = 0;
				while (dashes < text.size() && text[dashes] == '-')
					dashes++;
				Replace(c, std::string(dashes, '/') + std::string(text.substr(dashes)));
			}

			// The previous significant token makes the next an expression.
			bool ExpressionContext(size_t i) const
			{
				if (i == 0)
					return m_Expression;
				const Tok& p = m_Toks[i - 1];
				if (m_TypeEnd[i - 1] || m_BodyStart[i - 1])
					return false;
				if (p.K == Kind::Punct)
					return !(p.Is(")") || p.Is("]") || p.Is("}") || p.Is(";") || p.Is("...") || p.Is("?"));
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
					else if (t.Is("local") && !At(i + 1).Is("function"))
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

			static bool IsKeyword(std::string_view w)
			{
				static constexpr std::string_view kWords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
					"local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "continue" };
				return std::find(std::begin(kWords), std::end(kWords), w) != std::end(kWords);
			}

			// Token i can be an expression's last token (a value, a closing
			// bracket, a type a declaration ends with).
			bool EndsOperand(size_t i) const
			{
				const Tok& t = At(i);
				if (t.K == Kind::Number || t.K == Kind::String || m_TypeEnd[i])
					return true;
				if (t.K == Kind::Name)
					return !IsKeyword(t.Text) || t.Is("end") || t.Is("true") || t.Is("false") || t.Is("nil");
				return t.Is(")") || t.Is("]") || t.Is("}") || t.Is("...");
			}

			// A statement can start at token i.
			bool StatementStart(size_t i) const
			{
				if (i == 0)
					return !m_Expression;
				const Tok& p = At(i - 1);
				return p.Is(";") || p.Is("then") || p.Is("do") || p.Is("else") || p.Is("repeat") || p.Is("break") || p.Is("continue") ||
					m_BodyStart[i - 1] || EndsOperand(i - 1);
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
				for (const auto& [offset, length] : Segments(t.Text))
				{
					const std::string_view segment = t.Text.substr(offset, length);
					ConvertResult r = Converter(segment, true, m_Nesting + 1).Run();
					for (const Diagnostic& d : r.Diagnostics)
						Error(t, d.Message, d.Warning);
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

			void Token(size_t i)
			{
				const Tok& t = m_Toks[i];
				if (t.K == Kind::Punct)
				{
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
				if (t.K != Kind::Name)
					return;
				if (i > 0 && (At(i - 1).Is(".") || At(i - 1).Is(":")) && !At(i - 1).Is("::"))
					return; // a member name (`t.end` is not valid Luau, but `a.if` reads safely)
				const std::string_view w = t.Text;
				Frame* top = m_Stack.empty() ? nullptr : &m_Stack.back();
				if (w == "if")
				{
					if (ExpressionContext(i))
						m_Stack.push_back({ Block::IfExpr, true, i + 1 });
					else
						m_Stack.push_back({ Block::If, true, i + 1 });
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
						// (with the space before it when on the same line).
						Parenthesise(top->CondStart, i);
						const Tok& prev = At(i - 1);
						if (i > 0 && prev.Line == t.Line)
							m_Edits.push_back({ prev.End, t.End, "" });
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
					m_Stack.push_back({ Block::Loop, true, i + 1 });
				else if (w == "do")
				{
					if (top && top->B == Block::Loop && top->Awaiting)
					{
						Parenthesise(top->CondStart, i);
						top->Awaiting = false;
					}
					else
					{
						m_Stack.push_back({ Block::Do, false, 0 });
						// `local f do ... end`: `f {` would read as call sugar.
						const Tok& prev = At(i - 1);
						if (i > 0 && prev.Line == t.Line && (prev.K != Kind::Punct || prev.Is(")") || prev.Is("]") || prev.Is("}") || prev.Is("..."))
							&& !(prev.Is("then") || prev.Is("else") || prev.Is("do") || prev.Is("repeat")) && !m_BodyStart[i - 1])
						{
							Replace(t, "; {");
							return;
						}
					}
					Replace(t, "{");
				}
				else if (w == "repeat")
				{
					m_Stack.push_back({ Block::Repeat, false, 0 });
					Replace(t, "do {");
				}
				else if (w == "until")
					Until(i);
				else if (w == "function")
					Function(i);
				else if (w == "end")
				{
					while (top && top->B == Block::IfExpr)
					{
						m_Stack.pop_back();
						top = m_Stack.empty() ? nullptr : &m_Stack.back();
					}
					if (!top || top->B == Block::Repeat || top->Awaiting)
						Error(t, "'end' does not close a block");
					else
						m_Stack.pop_back();
					Replace(t, "}");
				}
				else if (w == "local")
				{
					if (!At(i + 1).Is("function"))
						Replace(t, "let");
				}
				else if (w == "and")
					Replace(t, "&&");
				else if (w == "or")
					Replace(t, "||");
				else if (w == "not")
				{
					const Tok& next = At(i + 1);
					if (next.Line == t.Line && next.Begin > t.End)
						m_Edits.push_back({ t.Begin, next.Begin, "!" });
					else
						Replace(t, "!");
				}
				else if (w == "let" || w == "const" || w == "class")
					Error(t, "'" + std::string(w) + "' is a cluau keyword: rename it (left as written)");
				else if ((w == "throw" || w == "try") && StatementStart(i) && At(i + 1).Line == t.Line &&
					(At(i + 1).Is("{") || (w == "throw" && (At(i + 1).Is("(") || At(i + 1).K == Kind::String))))
					Error(t, "a call to '" + std::string(w) + "' here reads as cluau's " + std::string(w) +
						" statement: rename the function (left as written)");
			}

			// `function [name][<generics>](params)[: type]` gets ` {` after its
			// signature.
			void Function(size_t i)
			{
				m_Stack.push_back({ Block::Function, false, 0 });
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

			// The index of the `>` closing the `<` at i.
			size_t SkipAngles(size_t i) const
			{
				int depth = 0;
				for (size_t j = i; j < m_Toks.size(); j++)
				{
					if (At(j).Is("<"))
						depth++;
					else if (At(j).Is(">") && --depth == 0)
						return j;
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
				else if (t.K == Kind::Name)
				{
					last = i;
					while (At(last + 1).Is(".") && At(last + 2).K == Kind::Name)
						last += 2;
					if (At(last + 1).Is("<"))
						last = SkipAngles(last + 1);
				}
				else
					return npos;
				while (At(last + 1).Is("?"))
					last++;
				return last;
			}

			// `until c` -> `} while (!(c));` (the condition runs to the end of
			// its line, or across lines while a bracket is open).
			void Until(size_t i)
			{
				const Tok& t = m_Toks[i];
				if (m_Stack.empty() || m_Stack.back().B != Block::Repeat)
				{
					Error(t, "'until' without a repeat");
					return;
				}
				m_Stack.pop_back();
				static constexpr std::string_view kStop[] = { "end", "else", "elseif", "until", "local", "return", "if", "while",
					"for", "repeat", "do", "break", "continue" };
				const size_t first = i + 1;
				size_t last = first;
				int depth = 0;
				int blocks = 0; // a function value's body (and the blocks in it) inside the condition
				int ifExprs = 0; // if-expressions in the condition waiting for their else
				for (size_t j = first; j < m_Toks.size(); j++)
				{
					const Tok& k = m_Toks[j];
					// The next statement ends the condition (before a `do` or
					// `if` it starts is counted as a block inside it).
					if (depth == 0 && blocks == 0 && j > first)
					{
						// A name or a number right after a finished operand starts
						// the next statement (`until n > 2 print(n)`).
						bool stop = k.Is(";") || k.Line != m_Toks[j - 1].Line ||
							(((k.K == Kind::Name && !(k.Is("and") || k.Is("or") || k.Is("then") || k.Is("else") || k.Is("elseif"))) ||
								 k.K == Kind::Number) &&
								EndsOperand(j - 1));
						for (std::string_view w : kStop)
							stop = stop || (k.K == Kind::Name && k.Text == w && !(w == "if" && ExpressionContext(j))
								&& !((w == "else" || w == "elseif") && ifExprs > 0));
						if (stop)
							break;
					}
					if (blocks == 0)
					{
						if (k.Is("if") && ExpressionContext(j))
							ifExprs++;
						else if (k.Is("else") && ifExprs > 0)
							ifExprs--;
					}
					if (k.K == Kind::Name)
					{
						if (k.Is("function") || k.Is("do") || k.Is("repeat") || (k.Is("if") && !ExpressionContext(j)))
							blocks++;
						else if ((k.Is("end") || k.Is("until")) && blocks > 0)
						{
							blocks--;
							last = j;
							continue;
						}
					}
					if (blocks > 0)
					{
						last = j;
						continue;
					}
					if (k.Is("(") || k.Is("[") || k.Is("{"))
						depth++;
					else if (k.Is(")") || k.Is("]") || k.Is("}"))
						depth--;
					last = j;
				}
				if (first >= m_Toks.size())
				{
					Error(t, "'until' without a condition");
					return;
				}
				// The source's own `;` ends the do-while (Luau refuses `;;`).
				const char* semi = At(last + 1).Is(";") ? "" : ";";
				// `until not (c)` reads best as `while (c)`.
				if (At(first).Is("not") && At(first + 1).Is("(") && m_Match[first + 1] == last && At(first + 1).Line == t.Line)
				{
					m_Skip[first] = true;
					m_Edits.push_back({ t.Begin, At(first + 1).Begin, "} while " });
					Insert(At(last).End, semi);
				}
				else
				{
					const bool grouped = At(first).Is("(") && m_Match[first] == last;
					// Through the space to the condition, when it is on this line.
					const size_t to = At(first).Line == t.Line ? At(first).Begin : t.End;
					m_Edits.push_back({ t.Begin, to, grouped ? "} while (!" : "} while (!(" });
					Insert(At(last).End, std::string(grouped ? ")" : "))") + semi);
				}
			}

			std::string_view m_Src;
			bool m_Expression = false;
			int m_Nesting = 0; // interpolated strings around this source
			std::vector<bool> m_TypeEnd;   // the last token of a type a statement can end with (`local x: T?`)
			std::vector<bool> m_BodyStart; // a function signature's last token (its body follows)
			std::vector<Tok> m_Toks;
			std::vector<Tok> m_Comments;
			std::vector<size_t> m_Match;
			std::vector<bool> m_Skip;
			std::vector<Frame> m_Stack;
			std::vector<Edit> m_Edits;
			std::vector<Diagnostic> m_Diagnostics;
			size_t m_LastIfExprWord = npos;
		};

	} // namespace

	bool ConvertResult::HasErrors() const
	{
		return std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const Diagnostic& d) { return !d.Warning; });
	}

	ConvertResult Convert(std::string_view luau)
	{
		return Converter(luau).Run();
	}

	namespace {

		// The tokens a round trip compares: no comments, parentheses or `;`,
		// and an interpolated string's segments as their own tokens (the
		// translator respells the operators in them).
		void Normalise(std::string_view text, std::vector<Tok>& out, int line = -1, int nesting = 0)
		{
			for (Tok t : Lex(text))
			{
				if (line >= 0)
					t.Line = line;
				if (t.K == Kind::Comment || t.Is("(") || t.Is(")") || t.Is(";"))
					continue;
				if (t.K == Kind::String && t.Text.front() == '`' && nesting < kMaxNesting)
				{
					size_t at = 0;
					for (const auto& [offset, length] : Segments(t.Text))
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
				out.push_back(t);
			}
		}

	} // namespace

	std::string VerifyConversion(std::string_view luau, std::string_view converted)
	{
		const TranslateResult back = Translate(converted);
		for (const Diagnostic& d : back.Diagnostics)
			if (!d.Warning)
				return "line " + std::to_string(d.At.Begin.Line + 1) + ": the conversion does not translate: " + d.Message;
		// A host's translation takes Luau's `error` on its first line (for
		// throw and try): not part of the round trip.
		std::string_view translated = back.Luau;
		constexpr std::string_view kRaiseHead = "local __cluau_error = error; ";
		if (translated.substr(0, kRaiseHead.size()) == kRaiseHead)
			translated.remove_prefix(kRaiseHead.size());
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
			if (i < a.size() && i < b.size() && a[i].Line != b[i].Line)
				return "line " + std::to_string(a[i].Line + 1) + ": '" + std::string(x) + "' moved to line " + std::to_string(b[i].Line + 1);
		}
		return {};
	}

} // namespace cluau
