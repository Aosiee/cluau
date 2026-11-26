#include "Lexer.h"

#include <cctype>

namespace cluau::detail {

	namespace {

		bool IsNameStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
		bool IsNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

		// The multi-character operators, longest first.
		constexpr std::string_view kOperators[] = {
			"...", "..=", "~/=", "//=", "??=",
			"==", "!=", "~=", "<=", ">=", "->", "::", "&&", "||", "~/", "..", "??",
			"+=", "-=", "*=", "/=", "%=", "^=", "++", "--",
		};

		class Lexer
		{
		public:
			Lexer(std::string_view source, std::vector<Diagnostic>& diagnostics, bool split)
				: m_Src(source), m_Diagnostics(diagnostics), m_Split(split)
			{
			}

			std::vector<Token> Run()
			{
				// A byte order mark is not code (the source's columns still count it).
				if (m_Src.substr(0, 3) == "\xEF\xBB\xBF")
					m_Pos = 3;
				for (;;)
				{
					SkipWhitespace();
					Token token = Start();
					if (m_Pos >= m_Src.size())
					{
						// An interpolated string still in a segment: reported at its start.
						if (!m_Interp.empty())
						{
							const Token& open = m_Tokens[m_Interp.front().Token];
							Diagnostic d;
							d.At = { { open.Line, open.Col }, { m_Line, Col() } };
							d.Message = "unterminated interpolated string";
							m_Diagnostics.push_back(std::move(d));
							for (const InterpOpen& o : m_Interp)
								m_Tokens[o.Token].Open = true;
						}
						token.Kind = TokenKind::Eof;
						Finish(token);
						m_Tokens.push_back(token);
						return std::move(m_Tokens);
					}
					m_TokenIndex = m_Tokens.size();
					m_PatternBracket = InPatternPosition();
					LexOne(token);
					m_TokenStartsPattern = m_PatternBracket;
					m_PatternBracket = false;
					Finish(token);
					m_Tokens.push_back(token);
					Track(m_Tokens.back());
				}
			}

		private:
			// The open segments of split interpolated strings: each one's
			// InterpBegin / InterpMid token and the braces open inside it.
			struct InterpOpen
			{
				size_t Token = 0;
				int Depth = 0;
			};

			Token Start() const
			{
				Token token;
				token.Begin = m_Pos;
				token.Line = m_Line;
				token.Col = Col();
				return token;
			}

			void Finish(Token& token) const
			{
				token.End = m_Pos;
				token.Text = m_Src.substr(token.Begin, token.End - token.Begin);
				token.EndLine = m_Line;
				token.EndCol = Col();
			}

			int Col() const { return (int)(m_Pos - m_LineStart); }
			char At(size_t offset = 0) const { return m_Pos + offset < m_Src.size() ? m_Src[m_Pos + offset] : '\0'; }

			void Advance()
			{
				if (m_Src[m_Pos] == '\n')
				{
					m_Line++;
					m_LineStart = m_Pos + 1;
				}
				m_Pos++;
			}

			void SkipWhitespace()
			{
				// Luau's whitespace, form feed and vertical tab included.
				while (m_Pos < m_Src.size() && (m_Src[m_Pos] == ' ' || m_Src[m_Pos] == '\t' || m_Src[m_Pos] == '\r' || m_Src[m_Pos] == '\n' ||
						   m_Src[m_Pos] == '\f' || m_Src[m_Pos] == '\v'))
					Advance();
			}

			void Error(const Token& at, std::string message)
			{
				Diagnostic d;
				d.At = { { at.Line, at.Col }, { m_Line, Col() } };
				d.Message = std::move(message);
				m_Diagnostics.push_back(std::move(d));
			}

			// The last token but `back` (comments skipped), or null.
			const Token* Previous(size_t back) const
			{
				for (size_t k = m_Tokens.size(); k-- > 0;)
					if (!m_Tokens[k].IsComment() && back-- == 0)
						return &m_Tokens[k];
				return nullptr;
			}

			// Where a destructuring pattern starts, or a nested one in it can:
			// after `let`, `const` or `local`, a loop variable's place in a
			// `for (...)` header, and inside a pattern after `[`, `,` or a
			// field's `:`. There `[[` is two brackets (`const [[a, b], c] =
			// pairs`), not a long string.
			bool InPatternPosition() const
			{
				const Token* prev = Previous(0);
				if (prev == nullptr)
					return false;
				if (prev->Is("let") || prev->Is("const") || prev->Is("local"))
					return true;
				if (m_PatternDepth > 0 && (prev->Is("[") || prev->Is(",") || prev->Is(":")))
					return true;
				return m_ForDepth == 1 && (prev->Is("(") || prev->Is(","));
			}

			// Follows the brackets of patterns and of `for` headers (up to
			// their `in` or `=`).
			void Track(const Token& t)
			{
				const bool opens = t.Is("(") || t.Is("[") || t.Is("{");
				const bool closes = t.Is(")") || t.Is("]") || t.Is("}");
				if (m_PatternDepth > 0)
					m_PatternDepth += opens ? 1 : closes ? -1 : 0;
				else if ((t.Is("[") || t.Is("{")) && m_TokenStartsPattern)
					m_PatternDepth = 1;
				if (m_ForDepth > 0)
				{
					m_ForDepth += opens ? 1 : closes ? -1 : 0;
					if (m_ForDepth == 1 && (t.Is("in") || t.Is("=")))
						m_ForDepth = 0;
				}
				else if (t.Is("("))
				{
					const Token* before = Previous(1);
					if (before != nullptr && before->Is("for"))
						m_ForDepth = 1;
				}
				m_TokenStartsPattern = false;
			}

			// `[` `=`* `[`: the level, or -1 when this is not a long bracket.
			int LongBracketLevel() const
			{
				if (At() != '[')
					return -1;
				size_t i = 1;
				while (At(i) == '=')
					i++;
				return At(i) == '[' ? (int)(i - 1) : -1;
			}

			// Consumes the long bracket's body up to `]` `=`*level `]`.
			bool SkipLongBracket(int level)
			{
				for (int i = 0; i < level + 2; i++)
					Advance();
				while (m_Pos < m_Src.size())
				{
					if (At() == ']')
					{
						size_t i = 1;
						while (At(i) == '=')
							i++;
						if ((int)(i - 1) == level && At(i) == ']')
						{
							for (size_t k = 0; k <= i; k++)
								Advance();
							return true;
						}
					}
					Advance();
				}
				return false;
			}

			// `continued`: the source ends where the string goes on (after a
			// `\` line continuation or a `\z`), so more input would finish it.
			bool SkipQuoted(char quote, bool* continued = nullptr)
			{
				Advance();
				bool going = false;
				while (m_Pos < m_Src.size())
				{
					const char c = At();
					if (c == '\\')
					{
						Advance();
						const bool skipSpace = m_Pos < m_Src.size() && At() == 'z';
						going = m_Pos >= m_Src.size() || skipSpace || At() == '\n' || At() == '\r';
						// A backslash before a line break continues the string
						// on the next line; a CRLF break is one break, as Luau
						// reads it.
						if (At() == '\r' && At(1) == '\n')
							Advance();
						if (m_Pos < m_Src.size())
							Advance();
						// `\z` skips the whitespace that follows, newlines included.
						while (skipSpace && m_Pos < m_Src.size() && std::isspace((unsigned char)At()))
							Advance();
						continue;
					}
					if (c == '\n')
						return false;
					going = false;
					Advance();
					if (c == quote)
						return true;
				}
				if (continued)
					*continued = going;
				return false;
			}

			enum class Part { End, Segment, Eof };

			// An interpolated string's text from here (after its backtick or a
			// segment's `}`) to its closing backtick or the next segment's `{`,
			// past either.
			Part SkipStringPart()
			{
				while (m_Pos < m_Src.size())
				{
					const char c = At();
					Advance();
					if (c == '\\')
					{
						if (m_Pos < m_Src.size())
							Advance();
					}
					else if (c == '`')
						return Part::End;
					else if (c == '{')
						return Part::Segment;
				}
				return Part::Eof;
			}

			// `...{expr}...` as one token: each segment is read as code (its
			// strings, comments and braces), up to the `}` that closes it.
			bool SkipInterpolated()
			{
				Advance();
				for (;;)
				{
					const Part part = SkipStringPart();
					if (part != Part::Segment)
						return part == Part::End;
					int depth = 0;
					m_Nesting++;
					for (;;)
					{
						SkipWhitespace();
						if (m_Pos >= m_Src.size())
						{
							m_Nesting--;
							return false;
						}
						if (At() == '}' && depth == 0)
						{
							Advance();
							break;
						}
						Token inner = Start();
						LexOne(inner);
						if (inner.Kind == TokenKind::Op && m_Src[inner.Begin] == '{')
							depth++;
						else if (inner.Kind == TokenKind::Op && m_Src[inner.Begin] == '}')
							depth--;
					}
					m_Nesting--;
				}
			}

			void LexOne(Token& token)
			{
				const char c = At();
				// Comments. A `#!` first line (a script's interpreter line) is
				// one too.
				if ((c == '/' && At(1) == '/') || (c == '#' && At(1) == '!' && m_Pos == 0))
				{
					token.Kind = TokenKind::LineComment;
					while (m_Pos < m_Src.size() && At() != '\n')
						Advance();
					return;
				}
				if (c == '/' && At(1) == '*')
				{
					token.Kind = TokenKind::BlockComment;
					Advance();
					Advance();
					while (m_Pos < m_Src.size() && !(At() == '*' && At(1) == '/'))
						Advance();
					if (m_Pos >= m_Src.size())
					{
						Error(token, "unterminated comment: /* needs a closing */");
						token.Open = true;
					}
					else
					{
						Advance();
						Advance();
					}
					return;
				}
				if (IsNameStart(c))
				{
					token.Kind = TokenKind::Name;
					while (m_Pos < m_Src.size() && IsNameChar(At()))
						Advance();
					return;
				}
				if (std::isdigit((unsigned char)c) || (c == '.' && std::isdigit((unsigned char)At(1))))
				{
					token.Kind = TokenKind::Number;
					while (m_Pos < m_Src.size())
					{
						const char d = At();
						if ((d == '+' || d == '-') && (m_Src[m_Pos - 1] == 'e' || m_Src[m_Pos - 1] == 'E')
							&& !(m_Src.size() > token.Begin + 1 && (m_Src[token.Begin + 1] == 'x' || m_Src[token.Begin + 1] == 'X')))
						{
							Advance();
							continue;
						}
						if (!(std::isalnum((unsigned char)d) || d == '_' || d == '.'))
							break;
						// `1..2` is a number, then `..`.
						if (d == '.' && At(1) == '.')
							break;
						Advance();
					}
					return;
				}
				if (c == '"' || c == '\'')
				{
					token.Kind = TokenKind::String;
					bool continued = false;
					if (!SkipQuoted(c, &continued))
					{
						Error(token, "unterminated string");
						token.Open = continued;
					}
					return;
				}
				if (c == '`' && m_Nesting + (int)m_Interp.size() >= kMaxInterpolationNesting)
				{
					// Reported once; the backtick is lexed as a stray character.
					if (!m_TooDeep)
					{
						Error(token, "interpolated strings nest too deeply (more than " + std::to_string(kMaxInterpolationNesting) + " levels)");
						m_TooDeep = true;
					}
					token.Kind = TokenKind::Op;
					Advance();
					return;
				}
				if (c == '`' && !m_Split)
				{
					token.Kind = TokenKind::Interpolated;
					if (!SkipInterpolated())
					{
						Error(token, "unterminated interpolated string");
						token.Open = true;
					}
					return;
				}
				if (c == '`' || (c == '}' && !m_Interp.empty() && m_Interp.back().Depth == 0))
				{
					// Split: a piece of text; the segment after it is lexed as code.
					const bool first = c == '`';
					Advance();
					const Part part = SkipStringPart();
					if (part == Part::Segment)
					{
						token.Kind = first ? TokenKind::InterpBegin : TokenKind::InterpMid;
						if (first)
							m_Interp.push_back({ m_TokenIndex, 0 });
						return;
					}
					token.Kind = first ? TokenKind::Interpolated : TokenKind::InterpEnd;
					size_t begin = m_TokenIndex;
					if (!first)
					{
						begin = m_Interp.back().Token;
						m_Interp.pop_back();
					}
					if (part == Part::Eof)
					{
						// Reported at the outermost string's start (Run, when one is open around it).
						token.Open = true;
						if (m_Interp.empty())
							Error(begin == m_TokenIndex ? token : m_Tokens[begin], "unterminated interpolated string");
					}
					return;
				}
				if (const int level = LongBracketLevel(); level >= 0 && !(m_PatternBracket && level == 0))
				{
					token.Kind = TokenKind::LongString;
					if (!SkipLongBracket(level))
					{
						Error(token, "unterminated long string");
						token.Open = true;
					}
					return;
				}
				token.Kind = TokenKind::Op;
				if (!m_Interp.empty() && (c == '{' || c == '}'))
					m_Interp.back().Depth += c == '{' ? 1 : -1;
				for (std::string_view op : kOperators)
					if (m_Src.compare(m_Pos, op.size(), op) == 0)
					{
						for (size_t i = 0; i < op.size(); i++)
							Advance();
						return;
					}
				Advance();
			}

			std::string_view m_Src;
			std::vector<Diagnostic>& m_Diagnostics;
			bool m_Split = false;
			std::vector<Token> m_Tokens;
			size_t m_TokenIndex = 0;          // the token being lexed
			std::vector<InterpOpen> m_Interp; // split: the segments open here
			int m_Nesting = 0;                // not split: the segments being skipped around here
			size_t m_Pos = 0;
			size_t m_LineStart = 0;
			int m_Line = 0;
			bool m_TooDeep = false;
			bool m_PatternBracket = false;     // the token being lexed is where a pattern may be
			bool m_TokenStartsPattern = false; // ... and so, a `[` or `{`, starts one
			int m_PatternDepth = 0;            // the brackets open in a destructuring pattern
			int m_ForDepth = 0;                // the brackets open in a `for (...)` header, before its `in` or `=`
		};

	} // namespace

	std::vector<Token> Lex(std::string_view source, std::vector<Diagnostic>& diagnostics, bool splitInterpolation)
	{
		return Lexer(source, diagnostics, splitInterpolation).Run();
	}

} // namespace cluau::detail
