#include "Lexer.h"

#include <cctype>

namespace cluau::detail {

	namespace {

		bool IsNameStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
		bool IsNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

		// The multi-character operators, longest first.
		constexpr std::string_view kOperators[] = {
			"...", "..=", "~/=", "//=",
			"==", "!=", "~=", "<=", ">=", "->", "::", "&&", "||", "~/", "..",
			"+=", "-=", "*=", "/=", "%=", "^=", "++", "--",
		};

		class Lexer
		{
		public:
			Lexer(std::string_view source, std::vector<Diagnostic>& diagnostics)
				: m_Src(source), m_Diagnostics(diagnostics)
			{
			}

			std::vector<Token> Run()
			{
				std::vector<Token> tokens;
				for (;;)
				{
					SkipWhitespace();
					Token token;
					token.Begin = m_Pos;
					token.Line = m_Line;
					token.Col = Col();
					if (m_Pos >= m_Src.size())
					{
						token.Kind = TokenKind::Eof;
						token.End = m_Pos;
						token.EndLine = m_Line;
						token.EndCol = Col();
						tokens.push_back(token);
						return tokens;
					}
					LexOne(token);
					token.End = m_Pos;
					token.Text = m_Src.substr(token.Begin, token.End - token.Begin);
					token.EndLine = m_Line;
					token.EndCol = Col();
					tokens.push_back(token);
				}
			}

		private:
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
				while (m_Pos < m_Src.size() && (m_Src[m_Pos] == ' ' || m_Src[m_Pos] == '\t' || m_Src[m_Pos] == '\r' || m_Src[m_Pos] == '\n'))
					Advance();
			}

			void Error(const Token& at, std::string message)
			{
				Diagnostic d;
				d.At = { { at.Line, at.Col }, { m_Line, Col() } };
				d.Message = std::move(message);
				m_Diagnostics.push_back(std::move(d));
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

			bool SkipQuoted(char quote)
			{
				Advance();
				while (m_Pos < m_Src.size())
				{
					const char c = At();
					if (c == '\\')
					{
						Advance();
						const bool skipSpace = m_Pos < m_Src.size() && At() == 'z';
						if (m_Pos < m_Src.size())
							Advance();
						// `\z` skips the whitespace that follows, newlines included.
						while (skipSpace && m_Pos < m_Src.size() && std::isspace((unsigned char)At()))
							Advance();
						continue;
					}
					if (c == '\n')
						return false;
					Advance();
					if (c == quote)
						return true;
				}
				return false;
			}

			// `...`, with `{expr}` segments that may hold strings and braces.
			bool SkipInterpolated()
			{
				Advance();
				int depth = 0;
				while (m_Pos < m_Src.size())
				{
					const char c = At();
					if (depth == 0)
					{
						if (c == '\\')
						{
							Advance();
							if (m_Pos < m_Src.size())
								Advance();
							continue;
						}
						if (c == '`')
						{
							Advance();
							return true;
						}
						if (c == '{')
							depth = 1;
						Advance();
						continue;
					}
					if (c == '"' || c == '\'')
					{
						SkipQuoted(c);
						continue;
					}
					if (c == '`')
					{
						SkipInterpolated();
						continue;
					}
					if (c == '{')
						depth++;
					else if (c == '}')
						depth--;
					Advance();
				}
				return false;
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
						Error(token, "unterminated comment: /* needs a closing */");
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
					if (!SkipQuoted(c))
						Error(token, "unterminated string");
					return;
				}
				if (c == '`')
				{
					token.Kind = TokenKind::Interpolated;
					if (!SkipInterpolated())
						Error(token, "unterminated interpolated string");
					return;
				}
				if (const int level = LongBracketLevel(); level >= 0)
				{
					token.Kind = TokenKind::LongString;
					if (!SkipLongBracket(level))
						Error(token, "unterminated long string");
					return;
				}
				token.Kind = TokenKind::Op;
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
			size_t m_Pos = 0;
			size_t m_LineStart = 0;
			int m_Line = 0;
		};

	} // namespace

	std::vector<Token> Lex(std::string_view source, std::vector<Diagnostic>& diagnostics)
	{
		return Lexer(source, diagnostics).Run();
	}

} // namespace cluau::detail
