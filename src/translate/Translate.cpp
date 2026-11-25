// cluau → Luau. One pass over the tokens decides each token's replacement
// (a statement keyword looks ahead to find its block's `{`; a stack of open
// brackets tells a block's `}` from a table's), then a second pass copies
// the source with the replacements. The text between tokens - whitespace
// and newlines - is always copied, so every line stays on its line.

#include "cluau/cluau.h"
#include "Lexer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace cluau {

	using detail::Token;
	using detail::TokenKind;

	namespace {

		constexpr size_t npos = (size_t)-1;

		enum class Frame : uint8_t { Paren, Bracket, Table, Block, Class };
		enum class BlockKind : uint8_t { Plain, If, Else, Loop, Function, Repeat, Method, Try, Catch, Finally };

		struct Open
		{
			Frame Kind = Frame::Paren;
			BlockKind Block = BlockKind::Plain;
			size_t Token = 0;
		};

		struct BlockMark
		{
			bool Set = false;
			BlockKind Kind = BlockKind::Plain;
			std::string Opener;
		};

		// Words that start or join an expression: a `{` after one is a table.
		bool IsExpressionWord(std::string_view w)
		{
			static constexpr const char* kWords[] = { "and", "or", "not", "return", "local", "let", "const", "in", "if", "while", "for",
				"else", "do", "function", "until", "then", "elseif", "type", "export", "class" };
			for (const char* k : kWords)
				if (w == k)
					return true;
			return false;
		}

		bool EndsExpression(const Token& t)
		{
			switch (t.Kind)
			{
			case TokenKind::Name: return !IsExpressionWord(t.Text);
			case TokenKind::Number:
			case TokenKind::String:
			case TokenKind::Interpolated:
			case TokenKind::LongString: return true;
			case TokenKind::Op: return t.Text == ")" || t.Text == "]" || t.Text == "}";
			default: return false;
			}
		}

		std::string Trim(std::string_view s)
		{
			size_t a = 0, b = s.size();
			while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
				a++;
			while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
				b--;
			return std::string(s.substr(a, b - a));
		}

		std::string Collapse(std::string_view s)
		{
			std::string out;
			bool space = false;
			for (char c : s)
			{
				if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
				{
					space = true;
					continue;
				}
				if (space && !out.empty())
					out += ' ';
				space = false;
				out += c;
			}
			return out;
		}

		void ReplaceAll(std::string& s, std::string_view from, std::string_view to)
		{
			for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
				s.replace(at, from.size(), to);
		}

		// The operator rewrite every expression gets.
		const char* OperatorReplacement(std::string_view op)
		{
			if (op == "!=") return "~=";
			if (op == "&&") return " and ";
			if (op == "||") return " or ";
			if (op == "~/") return "//";
			if (op == "~/=") return "//=";
			return nullptr;
		}

		// An expression's text with cluau's operators and if-expressions
		// rewritten (field defaults, interpolation segments).
		std::string TranslateExpressionText(std::string_view text)
		{
			std::vector<Diagnostic> ignored;
			std::vector<Token> tokens = detail::Lex(text, ignored);
			// An if-expression's condition is `if (` ... `)`: its `)` gains `then`.
			std::vector<uint8_t> conditionClose(tokens.size(), 0);
			for (size_t k = 0; k + 1 < tokens.size(); k++)
			{
				if (!tokens[k].Is("if") || !tokens[k + 1].Is("("))
					continue;
				int depth = 0;
				for (size_t m = k + 1; m < tokens.size(); m++)
				{
					if (tokens[m].Is("(") || tokens[m].Is("[") || tokens[m].Is("{"))
						depth++;
					else if ((tokens[m].Is(")") || tokens[m].Is("]") || tokens[m].Is("}")) && --depth == 0)
					{
						conditionClose[m] = 1;
						break;
					}
				}
			}
			std::string out;
			size_t pos = 0;
			for (size_t k = 0; k < tokens.size(); k++)
			{
				const Token& t = tokens[k];
				if (t.Kind == TokenKind::Eof)
					break;
				out.append(text.substr(pos, t.Begin - pos));
				pos = t.End;
				if (conditionClose[k])
				{
					out += ") then";
					continue;
				}
				if (t.Is("else") && k + 1 < tokens.size() && tokens[k + 1].Is("if"))
				{
					out += "elseif";
					pos = tokens[k + 1].End;
					k++;
					continue;
				}
				if (t.Kind == TokenKind::Op)
				{
					if (const char* r = OperatorReplacement(t.Text))
					{
						out += r;
						continue;
					}
					if (t.Text == "!")
					{
						out += "not ";
						continue;
					}
				}
				out.append(t.Text);
			}
			out.append(text.substr(pos));
			return out;
		}

		// `...{expr}...` with each segment's operators rewritten.
		std::string TranslateInterpolated(std::string_view text)
		{
			std::string out;
			size_t i = 0;
			while (i < text.size())
			{
				const char c = text[i];
				if (c == '\\' && i + 1 < text.size())
				{
					out += text.substr(i, 2);
					i += 2;
					continue;
				}
				if (c != '{')
				{
					out += c;
					i++;
					continue;
				}
				// The matching brace, skipping quoted strings.
				size_t j = i + 1;
				int depth = 1;
				while (j < text.size() && depth > 0)
				{
					const char d = text[j];
					if (d == '"' || d == '\'')
					{
						const char q = d;
						j++;
						while (j < text.size() && text[j] != q)
							j += text[j] == '\\' ? 2 : 1;
					}
					else if (d == '{')
						depth++;
					else if (d == '}')
						depth--;
					j++;
				}
				const size_t innerEnd = depth == 0 ? j - 1 : j;
				out += '{';
				out += TranslateExpressionText(text.substr(i + 1, innerEnd - i - 1));
				if (depth == 0)
					out += '}';
				i = j;
			}
			return out;
		}

		std::string ExpandClass(std::string text, const std::string& className)
		{
			ReplaceAll(text, "$Class", className);
			return text;
		}

		class Translator
		{
		public:
			Translator(std::string_view source, const TranslateOptions& options)
				: m_Src(source), m_Options(options)
			{
				m_Profile = options.Profile ? options.Profile : &m_DefaultProfile;
				if (m_Options.Repl)
					m_Options.Script = true;
			}

			TranslateResult Run()
			{
				m_Toks = detail::Lex(m_Src, m_R.Diagnostics);
				const size_t n = m_Toks.size();
				m_Repl.assign(n, std::nullopt);
				m_After.assign(n, std::string());
				m_BlockAt.assign(n, BlockMark{});
				m_OpenedBlock.assign(n, 0);
				m_ClosedBlock.assign(n, 0);
				m_ClosedIf.assign(n, 0);
				m_ClosedRepeat.assign(n, 0);
				m_ExprIfClose.assign(n, 0);
				m_Skip.assign(n, 0);
				Classify();
				Emit();
				return std::move(m_R);
			}

		private:
			// ---- helpers -------------------------------------------------

			size_t Sig(size_t j) const
			{
				while (j < m_Toks.size() && m_Toks[j].IsComment())
					j++;
				return std::min(j, m_Toks.size() - 1);
			}

			bool IsMember() const
			{
				return m_PrevSig != npos && (m_Toks[m_PrevSig].Is(".") || m_Toks[m_PrevSig].Is(":") || m_Toks[m_PrevSig].Is("::"));
			}

			// The bracket matching the one at j (any of ( [ { ; `<` for generics).
			size_t MatchClose(size_t j) const
			{
				const std::string_view open = m_Toks[j].Text;
				const bool angle = open == "<";
				int depth = 0;
				for (size_t k = j; k < m_Toks.size(); k++)
				{
					const Token& t = m_Toks[k];
					if (t.Kind != TokenKind::Op)
						continue;
					if (angle)
					{
						if (t.Text == "<")
							depth++;
						else if (t.Text == ">")
						{
							if (--depth == 0)
								return k;
						}
						continue;
					}
					if (t.Text == "(" || t.Text == "[" || t.Text == "{")
						depth++;
					else if (t.Text == ")" || t.Text == "]" || t.Text == "}")
					{
						if (--depth == 0)
							return k;
					}
				}
				return m_Toks.size() - 1;
			}

			// A type from j; the index after it.
			size_t SkipType(size_t j) const
			{
				j = SkipSimpleType(Sig(j));
				for (;;)
				{
					const size_t k = Sig(j);
					const Token& t = m_Toks[k];
					if (t.Is("?"))
						j = k + 1;
					else if (t.Is("|") || t.Is("&"))
						j = SkipSimpleType(Sig(k + 1));
					else if (t.Is("->"))
						j = SkipType(k + 1);
					else
						return j;
				}
			}

			size_t SkipSimpleType(size_t j) const
			{
				const Token& t = m_Toks[j];
				if (t.Is("(") || t.Is("{"))
					return MatchClose(j) + 1;
				if (t.Is("..."))
				{
					const size_t k = Sig(j + 1);
					return m_Toks[k].Kind == TokenKind::Name ? SkipSimpleType(k) : j + 1;
				}
				if (t.Kind == TokenKind::String || t.Kind == TokenKind::Number)
					return j + 1;
				if (t.Kind != TokenKind::Name)
					return j; // not a type: the caller reports what follows
				if (t.Text == "typeof")
				{
					const size_t p = Sig(j + 1);
					return m_Toks[p].Is("(") ? MatchClose(p) + 1 : j + 1;
				}
				size_t k = j + 1;
				while (m_Toks[Sig(k)].Is(".") && m_Toks[Sig(Sig(k) + 1)].Kind == TokenKind::Name)
					k = Sig(Sig(k) + 1) + 1;
				if (m_Toks[Sig(k)].Is("<"))
					k = MatchClose(Sig(k)) + 1;
				// A generic type pack: `R...`.
				if (m_Toks[Sig(k)].Is("..."))
					k = Sig(k) + 1;
				return k;
			}

			// The `{` that opens a header's block: the first `{` at the
			// header's own bracket depth.
			size_t FindHeaderBrace(size_t j) const
			{
				int depth = 0;
				for (size_t k = j; k < m_Toks.size(); k++)
				{
					const Token& t = m_Toks[k];
					if (t.Kind == TokenKind::Eof)
						return npos;
					if (t.Kind != TokenKind::Op)
						continue;
					if (t.Text == "{" && depth == 0)
						return k;
					if (t.Text == "(" || t.Text == "[" || t.Text == "{")
						depth++;
					else if (t.Text == ")" || t.Text == "]" || t.Text == "}")
					{
						if (--depth < 0)
							return npos;
					}
				}
				return npos;
			}

			std::string SourceText(size_t from, size_t to) const // tokens [from, to)
			{
				if (from >= to)
					return {};
				size_t last = to - 1;
				while (last > from && m_Toks[last].IsComment())
					last--;
				return std::string(m_Src.substr(m_Toks[from].Begin, m_Toks[last].End - m_Toks[from].Begin));
			}

			// Text the translation adds for `i` (all of it generated: the strict
			// rules do not report in it).
			void Generate(size_t i, std::string text)
			{
				m_GeneratedAt[i] = { 0, text.size() };
				m_Repl[i] = std::move(text);
			}

			// Generated text after token `i` (on its line).
			void AddAfter(size_t i, const std::string& text)
			{
				if (!m_Repl[i])
					m_Repl[i] = std::string(m_Toks[i].Text);
				m_After[i] += text;
			}

			// Ends a line that the next line continues.
			static bool AwaitsOperand(const Token& t)
			{
				if (t.Kind == TokenKind::Name)
					return t.Is("and") || t.Is("or") || t.Is("not");
				if (t.Kind != TokenKind::Op)
					return false;
				static constexpr const char* kOps[] = { "=", "+", "-", "*", "/", "%", "^", "..", "==", "!=", "~=", "<", "<=", ">", ">=", "&&",
					"||", "~/", ",", "!", "#", "(", "[", "{" };
				for (const char* op : kOps)
					if (t.Text == op)
						return true;
				return false;
			}

			// Starts a line that continues the one before.
			static bool ContinuesLine(const Token& t)
			{
				if (t.Kind == TokenKind::Name)
					return t.Is("and") || t.Is("or");
				if (t.Kind != TokenKind::Op)
					return false;
				static constexpr const char* kOps[] = { ".", ":", "..", "*", "/", "%", "^", "==", "!=", "~=", "<=", ">=", "&&", "||", "~/", "(", "[" };
				for (const char* op : kOps)
					if (t.Text == op)
						return true;
				return false;
			}

			// The last token of the expression (or list) a statement keyword at
			// `keyword` takes: to the end of its line unless brackets stay open or
			// the line goes on; npos when there is none.
			size_t ExpressionEnd(size_t keyword) const
			{
				size_t k = Sig(keyword + 1);
				const Token& first = m_Toks[k];
				if (first.Kind == TokenKind::Eof || first.Is("}") || first.Is(";") ||
					(first.Line > m_Toks[keyword].EndLine && !ContinuesLine(first)))
					return npos;
				int depth = 0;
				size_t last = npos;
				for (;; k = Sig(k + 1))
				{
					const Token& t = m_Toks[k];
					if (t.Kind == TokenKind::Eof)
						break;
					if (depth == 0 && (t.Is(";") || t.Is("}") || t.Is(")") || t.Is("]")))
						break;
					if (depth == 0 && last != npos && t.Line > m_Toks[last].EndLine && !AwaitsOperand(m_Toks[last]) && !ContinuesLine(t))
						break;
					if (t.Is("(") || t.Is("[") || t.Is("{"))
						depth++;
					else if (t.Is(")") || t.Is("]") || t.Is("}"))
						depth--;
					last = k;
				}
				return last;
			}

			void Error(const Token& t, std::string message, bool warning = false)
			{
				Diagnostic d;
				d.At = { { t.Line, t.Col }, { t.EndLine, t.EndCol } };
				d.Message = std::move(message);
				d.Warning = warning;
				m_R.Diagnostics.push_back(std::move(d));
			}

			void MarkBlock(size_t brace, BlockKind kind, std::string opener)
			{
				m_BlockAt[brace] = { true, kind, std::move(opener) };
			}

			bool InClassBody() const { return !m_Stack.empty() && m_Stack.back().Kind == Frame::Class; }

			// ---- the classify pass ---------------------------------------

			void Classify()
			{
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Token& t = m_Toks[i];
					if (t.Kind == TokenKind::Eof)
						break;
					if (t.IsComment())
					{
						Comment(i);
						continue;
					}
					if (m_Skip[i])
					{
						m_PrevSig = i;
						continue;
					}
					if (m_ClassEnded && !t.Is(";"))
					{
						Error(t, "a class must be the file's last statement");
						m_ClassEnded = false;
					}
					if (i > m_MethodHeaderEnd)
						m_MethodHeaderEnd = npos;
					if (m_StaticEnd != npos && i >= m_StaticEnd)
						m_StaticEnd = npos;
					if (InClassBody() && m_MethodHeaderEnd == npos && m_StaticEnd == npos && !t.Is("}") && !t.Is("function") && !t.Is(";"))
					{
						ClassItem(i);
						continue;
					}
					if (t.Kind == TokenKind::Name)
						Name(i);
					else if (t.Kind == TokenKind::Op)
						Operator(i);
					else if (t.Kind == TokenKind::Interpolated)
						m_Repl[i] = TranslateInterpolated(t.Text);
					m_PrevSig = i;
				}
				CloseAtEnd();
			}

			void Comment(size_t i)
			{
				const Token& t = m_Toks[i];
				if (t.Kind == TokenKind::LineComment)
				{
					if (t.Text.rfind("#!", 0) == 0)
					{
						// A script's interpreter line (`#!/usr/bin/env cluau`).
						m_Repl[i] = "--" + std::string(t.Text);
						return;
					}
					if (t.Text.size() > 2 && t.Text[2] == '!')
					{
						// A Luau hot comment (--!nonstrict) would change the
						// mode a host enforces: defused and reported.
						Error(t, "cluau files have no mode comments (//!...): the host decides the checking mode");
						m_Repl[i] = "-- " + std::string(t.Text.substr(2));
						return;
					}
					m_Repl[i] = "--" + std::string(t.Text.substr(2));
					const std::string body = Trim(t.Text.substr(2));
					if (body.rfind("allow-any", 0) == 0)
					{
						const size_t colon = body.find(':');
						const std::string reason = colon == std::string::npos ? std::string() : Trim(body.substr(colon + 1));
						if (reason.empty())
							Error(t, "an allow-any exemption needs a reason: // allow-any: <why any is needed here>");
						else
							m_R.Exemptions.push_back({ t.Line, reason });
					}
					return;
				}
				// /* ... */ → --[==[ ... ]==] at a level the body doesn't close.
				std::string_view body = t.Text.substr(2);
				if (body.size() >= 2 && body.substr(body.size() - 2) == "*/")
					body.remove_suffix(2);
				int level = 0;
				for (;; level++)
				{
					const std::string closer = "]" + std::string((size_t)level, '=') + "]";
					if (std::string_view(body).find(closer) == std::string_view::npos)
						break;
				}
				const std::string eq((size_t)level, '=');
				m_Repl[i] = "--[" + eq + "[" + std::string(body) + "]" + eq + "]";
			}

			void Name(size_t i)
			{
				const Token& t = m_Toks[i];
				if (IsMember())
					return;
				const std::string_view w = t.Text;
				if (w == "if")
				{
					if (IsExpressionPosition())
						ExpressionIf(i);
					else
						Header(i, BlockKind::If, "then", true);
				}
				else if (w == "while")
				{
					if (m_PrevSig != npos && m_ClosedRepeat[m_PrevSig])
					{
						// `} while (c)` closing a do-block: `until not (c)`.
						m_Repl[i] = "not";
						if (!m_Toks[Sig(i + 1)].Is("("))
							Error(t, "do { } while needs a parenthesised condition: } while (...);");
						return;
					}
					if (m_Options.Manifest)
						Error(t, "manifests are declarations: while loops are not allowed");
					Header(i, BlockKind::Loop, "do", true);
				}
				else if (w == "for")
					For(i);
				else if (w == "else")
					Else(i);
				else if (w == "do")
				{
					const size_t b = Sig(i + 1);
					if (!m_Toks[b].Is("{"))
					{
						Error(t, "do needs a block: do { ... } while (...);");
						return;
					}
					if (m_Options.Manifest)
						Error(t, "manifests are declarations: do { } while loops are not allowed");
					m_Repl[i] = "repeat";
					MarkBlock(b, BlockKind::Repeat, "");
				}
				else if (w == "function")
					Function(i);
				else if ((w == "let" || w == "local" || w == "const") && m_Options.Repl && m_Stack.empty())
					ReplDeclaration(i);
				else if (w == "let" || w == "local")
					m_Repl[i] = "local";
				else if (w == "const")
					Const(i);
				else if (w == "class")
					Class(i);
				else if (w == "abstract" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Is("class"))
				{
					Generate(i, "");
					m_AbstractClassNext = true;
				}
				else if (w == "super" && InClass())
					Super(i);
				else if (w == "throw" && IsStatementStart(i) && IsKeywordUse(i))
					Throw(i);
				else if (w == "throw" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Line != t.Line)
					Error(t, "throw raises a value: throw error / throw \"message\"");
				else if (w == "try" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Is("{") && m_Toks[Sig(i + 1)].Line == t.Line)
					Try(i);
				else if (w == "enum" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Kind == TokenKind::Name && m_Toks[Sig(Sig(i + 1) + 1)].Is("{"))
					Enum(i);
				else if (w == "catch" && m_PrevSig != npos && m_TryClosed.count(m_PrevSig) && m_TryClosed[m_PrevSig].second == BlockKind::Try)
					Catch(i);
				else if (w == "finally" && m_PrevSig != npos && m_TryClosed.count(m_PrevSig))
					Finally(i);
				else if (w == "return")
					Return(i);
				else if ((w == "break" || w == "continue") && IsStatementStart(i) && !IsKeywordUseAsName(i))
					Jump(i);
				else if (w == "then" || w == "end" || w == "elseif" || w == "repeat" || w == "until")
					Error(t, "cluau uses braces for blocks: '" + std::string(w) + "' is Luau syntax (see docs/syntax.md)");
			}

			// A contextual keyword (`throw`, `try`, `enum`, `abstract`) at a
			// statement's start is the keyword unless the statement uses it as a
			// name (`throw = 1`, `throw.x`, `throw(...)` across a line).
			bool IsKeywordUse(size_t i) const
			{
				const Token& next = m_Toks[Sig(i + 1)];
				if (next.Kind == TokenKind::Eof || next.Line != m_Toks[i].Line)
					return false;
				static constexpr const char* kNameUses[] = { "=", ".", ":", "[", ",", ";", "+=", "-=", "*=", "/=", "..=", "%=", "^=", "~/=", "++", "--" };
				for (const char* use : kNameUses)
					if (next.Is(use))
						return false;
				return true;
			}

			// `throw value`: raises `value` as it is (no position added, so a
			// re-raised error keeps its own): `error(value, 0)`.
			void Throw(size_t i)
			{
				const size_t last = ExpressionEnd(i);
				if (last == npos)
				{
					Error(m_Toks[i], "throw raises a value: throw error / throw \"message\"");
					return;
				}
				Generate(i, Raise() + "(");
				AddAfter(last, m_Options.Script ? ")" : ", 0)");
			}

			// What raises an error as it is: in a script, the runtime's
			// __cluau_throw; for a host, Luau's `error` (with level 0), taken by
			// each try before a `catch (error)` can hide the name.
			std::string Raise() const
			{
				if (m_Options.Script)
					return "__cluau_throw";
				const int t = EnclosingTry();
				return t < 0 ? "error" : TryName(m_Tries[(size_t)t], "raise");
			}

			std::string RaiseCall(const std::string& value) const
			{
				return m_Options.Script ? "__cluau_throw(" + value + ")" : Raise() + "(" + value + ", 0)";
			}

			// The innermost try, catch or finally block around here (-1: none).
			int EnclosingTry() const
			{
				for (size_t k = m_Stack.size(); k-- > 0;)
				{
					const Open& open = m_Stack[k];
					if (open.Kind == Frame::Block &&
						(open.Block == BlockKind::Try || open.Block == BlockKind::Catch || open.Block == BlockKind::Finally))
						if (auto it = m_TryOf.find(open.Token); it != m_TryOf.end())
							return (int)it->second;
				}
				return -1;
			}

			// `continue = 1`: Luau's `continue` is a name outside a statement of
			// its own.
			bool IsKeywordUseAsName(size_t i) const
			{
				const Token& next = m_Toks[Sig(i + 1)];
				return next.Line == m_Toks[i].Line && !(next.Is("}") || next.Is(";") || next.Kind == TokenKind::Eof);
			}

			// ---- enum -----------------------------------------------------------
			//
			// enum Color { Red, Green = "green", Blue } lowers, on its lines, to a
			// frozen table and its type:
			//   local Color = table.freeze({ Red = "Red" :: "Red", Green = "green" :: "green", Blue = "Blue" :: "Blue" });
			//   export type Color = "Red" | "green" | "Blue"
			// A member's value is its name, a string, or a number (then the type is
			// `number`: Luau has no number literal types).

			void Enum(size_t i)
			{
				const size_t nameTok = Sig(i + 1);
				const size_t brace = Sig(nameTok + 1);
				const size_t close = MatchClose(brace);
				const std::string name(m_Toks[nameTok].Text);
				std::vector<std::string> strings;
				std::set<std::string> names;
				bool numbers = false;
				for (size_t k = Sig(brace + 1); k < close;)
				{
					const Token& member = m_Toks[k];
					if (member.Is(","))
					{
						m_Skip[k] = 1;
						k = Sig(k + 1);
						continue;
					}
					if (member.Kind != TokenKind::Name || IsExpressionWord(member.Text) || member.Is("true") || member.Is("false") || member.Is("nil"))
					{
						Error(member, "an enum's members are names: enum " + name + " { First, Second }");
						break;
					}
					const std::string text(member.Text);
					if (!names.insert(text).second)
						Error(member, "enum " + name + " has '" + text + "' twice");
					size_t last = k;
					size_t next = Sig(k + 1);
					std::string value = "\"" + text + "\"";
					if (m_Toks[next].Is("="))
					{
						size_t v = Sig(next + 1);
						const bool negative = m_Toks[v].Is("-") && m_Toks[Sig(v + 1)].Kind == TokenKind::Number;
						if (negative)
							v = Sig(v + 1);
						if (m_Toks[v].Kind == TokenKind::String)
							value = std::string(m_Toks[v].Text);
						else if (m_Toks[v].Kind == TokenKind::Number)
						{
							value = std::string(m_Toks[v].Text);
							numbers = true;
						}
						else
						{
							Error(m_Toks[v], "an enum member's value is a string or a number: " + text + " = 1");
							break;
						}
						for (size_t s2 = next; s2 <= v; s2++)
							m_Skip[s2] = 1;
						last = v;
						next = Sig(v + 1);
					}
					m_Skip[k] = 1;
					const bool isString = value[0] == '"' || value[0] == '\'';
					if (isString)
					{
						strings.push_back(value);
						if (last == k)
							Generate(k, text + " = " + value + " :: " + value);
						else
							AddAfter(last, " :: " + value);
					}
					else if (last == k)
						Generate(k, text);
					// Members a line apart need no comma; the table does.
					if (!m_Toks[next].Is(",") && next < close)
						AddAfter(last, ",");
					k = next;
				}
				if (names.empty())
					Error(m_Toks[nameTok], "enum " + name + " has no members: enum " + name + " { First, Second }");
				std::string type;
				if (numbers && strings.empty())
					type = "number";
				else if (numbers)
					type = "string | number";
				else
					for (size_t k = 0; k < strings.size(); k++)
						type += (k ? " | " : "") + strings[k];
				if (type.empty())
					type = "never";
				Generate(i, m_Options.Repl ? "" : "local");
				Generate(nameTok, name + " = table.freeze(");
				m_Skip[nameTok] = 1;
				m_Skip[brace] = 1;
				m_Skip[close] = 1;
				AddAfter(close, "); " + std::string(m_Stack.empty() ? "export " : "") + "type " + name + " = " + type);
			}

			// ---- try / catch / finally ----------------------------------------
			//
			// try { A } catch (error) { B } finally { C } lowers, on its lines, to
			//   do local ok, sig, val = pcall(function() A end)
			//   if not ok then ok, sig, val = pcall(function(error: unknown) B end, sig) end
			//   do C end
			//   if not ok then error(sig, 0) end  (an error neither block handled)
			//   ... then what A or B left with: return (sig 1, its values in val),
			//   break (2), continue (3) ...
			// end
			// A `return`, `break` or `continue` in A or B leaves its closure with its
			// signal, and the statement then does it where it stands.

			struct TryState
			{
				int Id = 0;
				size_t Token = 0;
				bool Returns = false, Breaks = false, Continues = false;
				bool HasCatch = false, HasFinally = false;
			};

			std::string TryName(const TryState& t, const char* part) const { return "__try" + std::to_string(t.Id) + part; }

			void Try(size_t i)
			{
				if (m_Options.Manifest)
					Error(m_Toks[i], "manifests are declarations: try is not allowed");
				TryState state;
				state.Id = (int)m_Tries.size() + 1;
				state.Token = i;
				// A host's raise: Luau's `error`, or the enclosing try's (a catch
				// around this one may hide the name).
				const std::string raise = m_Options.Script ? std::string() : "local " + TryName(state, "raise") + " = " + Raise() + " ";
				m_Tries.push_back(state);
				const TryState& t = m_Tries.back();
				Generate(i, "do " + raise + "local " + TryName(t, "ok") + ": boolean, " + TryName(t, "sig") + ": any, " + TryName(t, "val") +
					": any = pcall(function(): ...any");
				const size_t brace = Sig(i + 1);
				MarkBlock(brace, BlockKind::Try, "");
				m_TryOf[brace] = m_Tries.size() - 1;
			}

			void Catch(size_t i)
			{
				const size_t index = m_TryClosed[m_PrevSig].first;
				TryState& t = m_Tries[index];
				t.HasCatch = true;
				Generate(i, " if not " + TryName(t, "ok") + " then " + TryName(t, "ok") + ", " + TryName(t, "sig") + ", " + TryName(t, "val") +
					" = pcall(function");
				const size_t open = Sig(i + 1);
				const size_t name = Sig(open + 1);
				if (!m_Toks[open].Is("(") || m_Toks[name].Kind != TokenKind::Name)
				{
					Error(m_Toks[i], "catch names the error it receives: catch (error) { }");
					return;
				}
				size_t close = Sig(name + 1);
				if (m_Toks[close].Is(":"))
					close = Sig(SkipType(close + 1));
				else
					AddAfter(name, ": unknown");
				const size_t brace = Sig(close + 1);
				if (!m_Toks[close].Is(")") || !m_Toks[brace].Is("{"))
				{
					Error(m_Toks[i], "catch is catch (error) { ... }");
					return;
				}
				AddAfter(close, ": ...any");
				MarkBlock(brace, BlockKind::Catch, "");
				m_TryOf[brace] = index;
			}

			void Finally(size_t i)
			{
				const size_t index = m_TryClosed[m_PrevSig].first;
				m_Tries[index].HasFinally = true;
				Generate(i, "");
				const size_t brace = Sig(i + 1);
				if (!m_Toks[brace].Is("{"))
				{
					Error(m_Toks[i], "finally needs a block: finally { ... }");
					return;
				}
				MarkBlock(brace, BlockKind::Finally, "do");
				m_TryOf[brace] = index;
			}

			// Where a return (or a break / continue) here goes: the try whose block
			// it leaves first (its index), or -1 when it reaches its target first
			// (the function for a return, a loop for a break or continue).
			// `finally` is set when a finally block is in the way, `loop` when the
			// target loop exists.
			int TryInTheWay(bool jump, bool& finally, bool& loop) const
			{
				finally = false;
				loop = false;
				int found = -1;
				for (size_t k = m_Stack.size(); k-- > 0;)
				{
					const Open& open = m_Stack[k];
					if (open.Kind == Frame::Class)
						break;
					if (open.Kind != Frame::Block)
						continue;
					if (open.Block == BlockKind::Function || open.Block == BlockKind::Method)
						break;
					if (open.Block == BlockKind::Finally && found < 0)
					{
						finally = true;
						return -1;
					}
					if (open.Block == BlockKind::Loop || open.Block == BlockKind::Repeat)
					{
						if (jump)
						{
							loop = true;
							return found;
						}
						continue;
					}
					if ((open.Block == BlockKind::Try || open.Block == BlockKind::Catch) && found < 0)
					{
						auto it = m_TryOf.find(open.Token);
						if (it != m_TryOf.end())
						{
							found = (int)it->second;
							if (!jump)
								return found;
						}
					}
				}
				return found;
			}

			void Return(size_t i)
			{
				bool finally = false, loop = false;
				const int index = TryInTheWay(false, finally, loop);
				if (finally)
				{
					Error(m_Toks[i], "a finally block runs on its way out: it cannot return");
					return;
				}
				if (index < 0)
					return;
				m_Tries[(size_t)index].Returns = true;
				const size_t last = ExpressionEnd(i);
				if (last == npos)
					Generate(i, "return 1, table.pack()");
				else
				{
					Generate(i, "return 1, table.pack(");
					AddAfter(last, ")");
				}
			}

			void Jump(size_t i)
			{
				const bool isBreak = m_Toks[i].Is("break");
				bool finally = false, loop = false;
				const int index = TryInTheWay(true, finally, loop);
				if (finally)
				{
					Error(m_Toks[i], std::string("a finally block runs on its way out: it cannot ") + (isBreak ? "break" : "continue"));
					return;
				}
				if (index < 0)
					return;
				if (!loop)
				{
					Error(m_Toks[i], std::string(isBreak ? "break" : "continue") + " is not inside a loop");
					return;
				}
				(isBreak ? m_Tries[(size_t)index].Breaks : m_Tries[(size_t)index].Continues) = true;
				Generate(i, isBreak ? "return 2" : "return 3");
			}

			// After the try's last block: an error left over is raised again, and a
	// return / break / continue from inside happens here (or travels on, out
	// of an enclosing try). `last`: the statement ends its function, so its
	// return is unconditional (the function returns on every path).
	std::string TryEnd(const TryState& t, bool last)
	{
		const std::string ok = TryName(t, "ok"), sig = TryName(t, "sig"), val = TryName(t, "val");
		std::string out = " if not " + ok + " then " + (m_Options.Script ? RaiseCall(sig) : TryName(t, "raise") + "(" + sig + ", 0)") + " end";
		bool finally = false, loop = false;
		for (const auto& [wanted, code, word] : { std::tuple{ t.Breaks, 2, "break" }, std::tuple{ t.Continues, 3, "continue" } })
		{
			if (!wanted)
				continue;
			const int outer = TryInTheWay(true, finally, loop);
			if (outer >= 0)
			{
				(code == 2 ? m_Tries[(size_t)outer].Breaks : m_Tries[(size_t)outer].Continues) = true;
				out += " if " + sig + " == " + std::to_string(code) + " then return " + std::to_string(code) + " end";
			}
			else
				out += " if " + sig + " == " + std::to_string(code) + " then " + word + " end";
		}
		if (t.Returns)
		{
			const int outer = TryInTheWay(false, finally, loop);
			if (outer >= 0)
			{
				m_Tries[(size_t)outer].Returns = true;
				out += " if " + sig + " == 1 then return 1, " + val + " end";
			}
			else if (last)
				out += " return table.unpack(" + val + " or {}, 1, if " + sig + " == 1 then " + val + ".n else 0)";
			else
				out += " if " + sig + " == 1 then return table.unpack(" + val + ", 1, " + val + ".n) end";
		}
		return out + " end";
	}

	void Header(size_t i, BlockKind kind, const char* opener, bool parenthesised)
			{
				const Token& t = m_Toks[i];
				const size_t c = Sig(i + 1);
				if (parenthesised && !m_Toks[c].Is("("))
					Error(t, "cluau conditions are parenthesised: " + std::string(t.Text) + " (...) { }");
				const size_t brace = FindHeaderBrace(c);
				if (brace == npos)
				{
					Error(t, std::string(t.Text) + " needs a { } block");
					return;
				}
				MarkBlock(brace, kind, opener);
			}

			void For(size_t i)
			{
				const Token& t = m_Toks[i];
				const size_t p = Sig(i + 1);
				if (!m_Toks[p].Is("("))
				{
					Error(t, "cluau loops are parenthesised: for (i = 1, n) { } / for (k, v in t) { }");
					Header(i, BlockKind::Loop, "do", false);
					return;
				}
				const size_t q = MatchClose(p);
				const size_t brace = Sig(q + 1);
				if (!m_Toks[brace].Is("{"))
				{
					Error(t, "for needs a { } block after its (...)");
					return;
				}
				// The parentheses are not Luau: blank them (same width).
				m_Repl[p] = " ";
				m_Repl[q] = " ";
				if (const size_t let = Sig(p + 1); m_Toks[let].Is("let"))
				{
					m_Repl[let] = "";
					m_Skip[let] = 1;
				}
				MarkBlock(brace, BlockKind::Loop, "do");
			}

			void Else(size_t i)
			{
				const Token& t = m_Toks[i];
				const bool afterIfBlock = m_PrevSig != npos && m_ClosedIf[m_PrevSig];
				if (!m_ExprIfs.empty() && m_ExprIfs.back().Depth == m_Stack.size())
				{
					if (!afterIfBlock)
					{
						ExpressionElse(i);
						return;
					}
					MissingElse();
				}
				if (!afterIfBlock)
					Error(t, "else without an if block before it");
				const size_t next = Sig(i + 1);
				if (m_Toks[next].Is("if"))
				{
					m_Repl[i] = "elseif";
					m_Repl[next] = "";
					m_Skip[next] = 1;
					Header(next, BlockKind::If, "then", true);
					return;
				}
				const size_t brace = Sig(i + 1);
				if (!m_Toks[brace].Is("{"))
				{
					Error(t, "else needs a { } block");
					return;
				}
				MarkBlock(brace, BlockKind::Else, "");
			}

			// An `if` where an expression is expected (after `=`, `(`, `,`,
			// an operator, `return`, ...) is an if-expression:
			// `if (c) a else if (d) b else e` lowers to Luau's
			// `if (c) then a elseif (d) then b else e`, every token in place.
			bool IsExpressionPosition() const
			{
				if (m_PrevSig == npos || m_OpenedBlock[m_PrevSig] || m_ClosedBlock[m_PrevSig])
					return false;
				if (m_ExprIfClose[m_PrevSig])
					return true; // if (a) if (b) 1 else 2 else 3
				const Token& p = m_Toks[m_PrevSig];
				if (p.Kind == TokenKind::Op)
					return !(p.Is(")") || p.Is("]") || p.Is("}") || p.Is(";"));
				if (p.Kind == TokenKind::Name)
					return p.Is("return") || p.Is("and") || p.Is("or") || p.Is("not") || p.Is("in");
				return false;
			}

			bool ExpressionCondition(size_t ifToken)
			{
				const size_t c = Sig(ifToken + 1);
				if (!m_Toks[c].Is("("))
				{
					Error(m_Toks[ifToken], "an if-expression's condition is parenthesised: if (c) a else b");
					return false;
				}
				const size_t close = MatchClose(c);
				m_Repl[close] = ") then";
				m_ExprIfClose[close] = 1;
				return true;
			}

			void ExpressionIf(size_t i)
			{
				if (ExpressionCondition(i))
					m_ExprIfs.push_back({ m_Stack.size(), i });
			}

			void ExpressionElse(size_t i)
			{
				const size_t next = Sig(i + 1);
				if (m_Toks[next].Is("if"))
				{
					m_Repl[i] = "elseif";
					m_Repl[next] = "";
					m_Skip[next] = 1;
					if (ExpressionCondition(next))
						return;
				}
				m_ExprIfs.pop_back();
			}

			// The pending if-expression ended without its `else`.
			void MissingElse()
			{
				Error(m_Toks[m_ExprIfs.back().If], "an if-expression needs an else: if (c) a else b");
				m_ExprIfs.pop_back();
			}

			void EndExpressionIfsAbove(size_t depth)
			{
				while (!m_ExprIfs.empty() && m_ExprIfs.back().Depth > depth)
					MissingElse();
			}

			void Function(size_t i)
			{
				const Token& t = m_Toks[i];
				if (m_Options.Manifest)
					Error(t, "manifests are declarations: function definitions are not allowed");
				const bool method = InClassBody();
				size_t j = Sig(i + 1);
				if (method)
				{
					if (m_Toks[j].Kind != TokenKind::Name)
					{
						Error(t, "a method needs a name: function Name() { }");
						return;
					}
					m_Repl[j] = m_ClassName + "." + std::string(m_Toks[j].Text);
				}
				while (m_Toks[j].Kind == TokenKind::Name)
				{
					const size_t k = Sig(j + 1);
					if (m_Toks[k].Is(".") || m_Toks[k].Is(":"))
					{
						j = Sig(k + 1);
						continue;
					}
					j = k;
					break;
				}
				if (m_Toks[j].Is("<"))
					j = Sig(MatchClose(j) + 1);
				if (!m_Toks[j].Is("("))
				{
					Error(t, "a function needs a parameter list");
					return;
				}
				const size_t close = MatchClose(j);
				if (method)
					m_Repl[j] = "(self: " + m_ClassName + (Sig(j + 1) == close ? "" : ", ");
				size_t k = Sig(close + 1);
				if (m_Toks[k].Is(":"))
					k = Sig(SkipType(k + 1));
				if (method && i == m_AbstractFunction)
				{
					// The body a subclass replaces: an error naming what is missing.
					// (A `return` only with a declared return type: the call's own type,
					// never, would otherwise become the method's, and an override's.)
					const bool returns = m_Toks[Sig(close + 1)].Is(":");
					AddAfter(m_AbstractEnd, std::string(returns ? " return " : " ") + "__cluau_abstract(\"" + m_ClassName + "\", \"" +
						std::string(m_Toks[Sig(i + 1)].Text) + "\") end");
					m_MethodHeaderEnd = m_AbstractEnd;
					m_AbstractFunction = npos;
					return;
				}
				if (!m_Toks[k].Is("{"))
				{
					Error(m_Toks[k], "a function body is a { } block");
					return;
				}
				MarkBlock(k, method ? BlockKind::Method : BlockKind::Function, "");
				if (method)
					m_MethodHeaderEnd = k; // the header's tokens are not class items
			}

			void Const(size_t i)
			{
				m_Repl[i] = "local";
				size_t j = Sig(i + 1);
				while (m_Toks[j].Kind == TokenKind::Name)
				{
					const Token& name = m_Toks[j];
					m_R.ConstNames.push_back({ { name.Line, name.Col }, { name.EndLine, name.EndCol } });
					j = Sig(j + 1);
					if (m_Toks[j].Is(":"))
						j = Sig(SkipType(j + 1));
					if (!m_Toks[j].Is(","))
						break;
					j = Sig(j + 1);
				}
			}

			// A top-level declaration in REPL mode assigns globals:
			// `let x: T = v` -> `x = v`, `let x` -> `x = nil`,
			// `local function f` -> `function f`.
			void ReplDeclaration(size_t i)
			{
				m_Repl[i] = "";
				size_t j = Sig(i + 1);
				if (m_Toks[j].Is("function"))
					return;
				size_t last = npos;
				while (m_Toks[j].Kind == TokenKind::Name)
				{
					last = j;
					j = Sig(j + 1);
					if (m_Toks[j].Is(":"))
					{
						const size_t typeEnd = SkipType(j + 1);
						for (size_t k = j; k < typeEnd; k++)
							if (!m_Toks[k].IsComment())
							{
								m_Repl[k] = "";
								m_Skip[k] = 1;
							}
						j = Sig(typeEnd);
					}
					if (!m_Toks[j].Is(","))
						break;
					j = Sig(j + 1);
				}
				if (last != npos && !m_Toks[j].Is("="))
					m_Repl[last] = std::string(m_Toks[last].Text) + " = nil";
			}

			// ---- classes -------------------------------------------------

			bool InClass() const
			{
				for (const Open& open : m_Stack)
					if (open.Kind == Frame::Class)
						return true;
				return false;
			}

			// In a subclass's methods: `super:Method(args)` calls the base's
			// method on self (`Base.Method(self, args)`), `super.Name` is the base's
			// member.
			void Super(size_t i)
			{
				const Token& t = m_Toks[i];
				if (m_ClassBase.empty())
				{
					Error(t, "super needs a base class: class " + m_ClassName + " extends Base { }");
					return;
				}
				const size_t next = Sig(i + 1);
				if (m_Toks[next].Is(":"))
				{
					const size_t name = Sig(next + 1);
					const size_t open = Sig(name + 1);
					if (m_Toks[name].Kind == TokenKind::Name && m_Toks[open].Is("("))
					{
						m_Repl[i] = m_ClassBase;
						m_Repl[next] = ".";
						m_Repl[open] = m_Toks[Sig(open + 1)].Is(")") ? "(self" : "(self, ";
						return;
					}
				}
				else if (m_Toks[next].Is("."))
				{
					m_Repl[i] = m_ClassBase;
					return;
				}
				Error(t, "super is super:Method(...) (the base class's method, on self) or super.Name");
			}

			struct FieldSpan
			{
				size_t Begin = 0;
				size_t End = 0; // exclusive
				ClassField Field;
			};

			void Class(size_t i)
			{
				const Token& t = m_Toks[i];
				if (m_Options.Manifest)
				{
					Error(t, "manifests are declarations: classes are not allowed");
					return;
				}
				if (m_R.Class && !m_Options.Script)
				{
					Error(t, "a file declares at most one class");
					return;
				}
				if (!m_Stack.empty())
				{
					Error(t, "a class is declared at the top level of the file");
					return;
				}
				const size_t nameTok = Sig(i + 1);
				if (m_Toks[nameTok].Kind != TokenKind::Name)
				{
					Error(t, "a class needs a name: class Name { }");
					return;
				}
				size_t brace = Sig(nameTok + 1);
				// `extends Base` (a name or a dotted path: `shapes.Square`).
				std::string base;
				Range baseAt;
				const size_t extendsTok = m_Toks[brace].Is("extends") ? brace : npos;
				if (extendsTok != npos)
				{
					size_t k = Sig(extendsTok + 1);
					if (m_Toks[k].Kind == TokenKind::Name)
					{
						baseAt.Begin = { m_Toks[k].Line, m_Toks[k].Col };
						base = std::string(m_Toks[k].Text);
						size_t last = k;
						k = Sig(k + 1);
						while (m_Toks[k].Is(".") && m_Toks[Sig(k + 1)].Kind == TokenKind::Name)
						{
							last = Sig(k + 1);
							base += "." + std::string(m_Toks[last].Text);
							k = Sig(last + 1);
						}
						baseAt.End = { m_Toks[last].EndLine, m_Toks[last].EndCol };
					}
					else
						Error(m_Toks[extendsTok], "extends names the base class: class Name extends Base { }");
					if (!m_Profile->Inheritance)
						Error(m_Toks[extendsTok], "this host's classes do not extend other classes");
					for (size_t j = extendsTok; j < k; j++)
						if (!m_Toks[j].IsComment())
						{
							m_Repl[j] = "";
							m_Skip[j] = 1;
						}
					brace = k;
				}
				if (!m_Toks[brace].Is("{"))
				{
					Error(m_Toks[nameTok], "a class body is a { } block");
					return;
				}
				const size_t end = MatchClose(brace);
				m_ClassName = std::string(m_Toks[nameTok].Text);
				m_ClassBase = base;

				ClassInfo info;
				info.Name = m_ClassName;
				info.At = { { m_Toks[nameTok].Line, m_Toks[nameTok].Col }, { m_Toks[nameTok].EndLine, m_Toks[nameTok].EndCol } };
				info.Base = base;
				info.BaseAt = baseAt;
				info.Abstract = m_AbstractClassNext;
				m_AbstractClassNext = false;
				if (info.Abstract && !m_Profile->Inheritance)
					Error(t, "this host's classes are not abstract (or extended)");
				info.BodyBeginLine = m_Toks[brace].Line;
				info.BodyEndLine = m_Toks[end].Line;
				ScanClass(brace, end, info);

				// Abstract methods: declared by an abstract class, and each one defined
				// by a class that is not (when its bases are in this file).
				std::set<std::string> missing;
				if (auto inherited = m_Unimplemented.find(base); inherited != m_Unimplemented.end())
					missing = inherited->second;
				for (const ClassMethod& m : info.Methods)
				{
					if (m.Abstract)
					{
						missing.insert(m.Name);
						if (!info.Abstract)
							Error(m_Toks[nameTok], m.Name + " is abstract, so " + m_ClassName + " is too: abstract class " + m_ClassName);
					}
					else
						missing.erase(m.Name);
				}
				m_Unimplemented[m_ClassName] = missing;
				std::string list; // the inherited ones still missing (an own abstract method is reported above)
				for (const std::string& name : missing)
				{
					bool own = false;
					for (const ClassMethod& m : info.Methods)
						own = own || (m.Abstract && m.Name == name);
					if (!own)
						list += (list.empty() ? "" : ", ") + name;
				}
				if (!info.Abstract && !list.empty())
				{
					Error(m_Toks[nameTok], m_ClassName + " does not define " + list + ", which " + (base.empty() ? std::string("it") : base) +
						" declares abstract");
				}

				// The header: the module table and the class type, on the
				// class line.
				ClassMembers members;
				members.Fields = ExpandClass(m_Profile->BaseMembers, m_ClassName);
				for (const ClassField& field : info.Fields)
				{
					const std::string luauType = m_Profile->FieldType ? m_Profile->FieldType(field) : field.Type;
					members.Fields += field.Name + ": " + (luauType.empty() ? "any" : luauType) + ", ";
				}
				if (m_Profile->SignalType)
					for (const ClassSignal& signal : info.Signals)
						members.Fields += signal.Name + ": " + m_Profile->SignalType(signal) + ", ";
				for (const ClassMethod& m : info.Methods)
				{
					members.Methods.push_back({ m.Name, MethodParamTypes(m.Params),
						!m.ReturnType.empty() ? m.ReturnType : m.ReturnsValue ? std::string("...any") : std::string("()") });
					if (m.Name == "Init")
						members.CtorParams = members.Methods.back().ParamTypes;
				}
				if (!members.CtorParams)
				{
					if (base.empty())
						members.CtorParams = std::string();
					else if (auto inherited = m_CtorParams.find(base); inherited != m_CtorParams.end())
						members.CtorParams = inherited->second;
				}
				m_CtorParams[m_ClassName] = members.CtorParams;
	info.UncheckedConstructor = !members.CtorParams;
				std::string type;
				std::string head;
				if (m_Profile->Header)
				{
					ClassHeader custom = m_Profile->Header(info, members);
					head = m_ClassName + " = " + ExpandClass(custom.TableInit, m_ClassName) + "; ";
					type = ExpandClass(custom.Types, m_ClassName);
				}
				else
				{
					type = "export type " + m_ClassName + " = { " + members.Fields;
					for (const ClassMembers::Method& m : members.Methods)
						type += m.Name + ": (self: " + m_ClassName + m.ParamTypes + ") -> " + m.ReturnType + ", ";
					type += "}";
					head = m_ClassName + " = " + ExpandClass(m_Profile->TableInit, m_ClassName) + "; ";
				}
				m_Repl[i] = m_Options.Repl ? "" : "local";
				std::string signals;
				if (!info.Signals.empty())
				{
					const std::string& member = m_Profile->SignalsMember;
					if (member.empty())
						Error(m_Toks[nameTok], "this host's classes declare no signals");
					else
					{
						for (const std::string& name : info.Statics)
							if (name == member)
								Error(m_Toks[nameTok], "declare the signals with `signal Name(...)`, not `static " + member + "`");
						signals = "; " + m_ClassName + "." + member + " = {";
						for (size_t k = 0; k < info.Signals.size(); k++)
							signals += (k ? ", \"" : " \"") + info.Signals[k].Name + "\"";
						signals += " }";
					}
				}
				m_Repl[nameTok] = head + type + signals;
				if (m_Profile->Header)
					m_GeneratedAt[nameTok] = { 0, head.size() + type.size() };
				else
					m_GeneratedAt[nameTok] = { head.size(), type.size() };
				m_Repl[brace] = "";
				m_Stack.push_back({ Frame::Class, BlockKind::Plain, brace });
				m_OpenedBlock[brace] = 1;
				m_ClassEndToken = end;
				m_R.Classes.push_back(info);
				if (!m_R.Class)
					m_R.Class = std::move(info);
				m_Skip[nameTok] = 1;
				m_Skip[brace] = 1;
			}

			// `a: number, b` → ", a: number, b: any" (a function type's list).
			static std::string MethodParamTypes(const std::string& params)
			{
				std::string out;
				int depth = 0;
				size_t start = 0;
				auto add = [&](std::string p) {
					p = Trim(p);
					if (p.empty())
						return;
					const size_t colon = p.find(':');
					if (p.rfind("...", 0) == 0)
						out += ", ..." + (colon == std::string::npos ? std::string("any") : Trim(p.substr(colon + 1)));
					else
						out += ", " + (colon == std::string::npos ? p + ": any" : p);
				};
				for (size_t k = 0; k < params.size(); k++)
				{
					const char c = params[k];
					if (c == '(' || c == '{' || c == '<' || c == '[')
						depth++;
					else if (c == ')' || c == '}' || c == '>' || c == ']')
						depth--;
					else if (c == ',' && depth == 0)
					{
						add(params.substr(start, k - start));
						start = k + 1;
					}
				}
				add(params.substr(start));
				return out;
			}

			void ScanClass(size_t brace, size_t end, ClassInfo& info)
			{
				size_t j = Sig(brace + 1);
				while (j < end)
				{
					const Token& t = m_Toks[j];
					if (t.Is(";"))
					{
						j = Sig(j + 1);
						continue;
					}
					std::vector<std::string> attributes;
					const size_t begin = j;
					while (m_Toks[j].Is("@") && j < end)
					{
						const size_t a = Sig(j + 1);
						if (m_Toks[a].Kind == TokenKind::Name)
							attributes.push_back(std::string(m_Toks[a].Text));
						j = Sig(a + 1);
					}
					if (m_Toks[j].Is("abstract") && m_Toks[Sig(j + 1)].Is("function"))
					{
						j = ScanAbstract(j, end, info);
						continue;
					}
					if (m_Toks[j].Is("function"))
					{
						j = ScanMethod(j, end, info);
						continue;
					}
					if (m_Toks[j].Is("static"))
					{
						j = ScanStatic(j, end, info);
						continue;
					}
					if (m_Toks[j].Is("signal") && m_Toks[Sig(j + 1)].Kind == TokenKind::Name)
					{
						j = ScanSignal(j, end, info);
						continue;
					}
					if (m_Toks[j].Kind != TokenKind::Name)
					{
						Error(m_Toks[j], "a class body holds fields (Name: Type = value) and methods (function Name() { })");
						j = Sig(j + 1);
						continue;
					}
					FieldSpan span;
					span.Begin = begin;
					span.Field.Name = std::string(m_Toks[j].Text);
					span.Field.Attributes = std::move(attributes);
					span.Field.At = { { m_Toks[j].Line, m_Toks[j].Col }, { m_Toks[j].EndLine, m_Toks[j].EndCol } };
					size_t k = Sig(j + 1);
					if (m_Toks[k].Is(":"))
					{
						const size_t typeBegin = Sig(k + 1);
						const size_t typeEnd = SkipType(typeBegin);
						span.Field.Type = Collapse(SourceText(typeBegin, typeEnd));
						k = Sig(typeEnd);
					}
					else
						Error(m_Toks[j], "a field declares its type: " + span.Field.Name + ": Type = value");
					if (m_Toks[k].Is("="))
					{
						// The default runs to the end of its line, unless brackets stay open.
						const size_t valueBegin = Sig(k + 1);
						size_t v = valueBegin;
						int depth = 0;
						while (v < end)
						{
							const Token& vt = m_Toks[v];
							if (vt.Kind == TokenKind::Eof)
								break;
							if (depth == 0 && v > valueBegin && !vt.IsComment() && vt.Line > m_Toks[PrevSig(v)].EndLine)
								break;
							if (depth == 0 && vt.Is(";"))
								break;
							if (vt.Is("(") || vt.Is("[") || vt.Is("{"))
								depth++;
							else if (vt.Is(")") || vt.Is("]") || vt.Is("}"))
								depth--;
							v++;
						}
						span.Field.Default = Collapse(TranslateExpressionText(SourceText(valueBegin, v)));
						k = v;
					}
					// Trailing comments stay where they are.
					while (k > begin && m_Toks[k - 1].IsComment())
						k--;
					span.End = std::max(k, Sig(j + 1));
					info.Fields.push_back(span.Field);
					m_FieldSpans.push_back(span);
					j = Sig(span.End);
				}
			}

			// `static Name = expression`: a member of the module table, not of
			// an instance (Class.Name = expression). The expression runs to the
			// end of its line unless brackets stay open; the main pass
			// translates it like any other expression.
			size_t ScanStatic(size_t j, size_t end, ClassInfo& info)
			{
				const size_t nameTok = Sig(j + 1);
				const size_t eq = Sig(nameTok + 1);
				if (m_Toks[nameTok].Kind != TokenKind::Name || !m_Toks[eq].Is("="))
				{
					Error(m_Toks[j], "a static member is: static Name = value");
					return Sig(j + 1);
				}
				size_t v = Sig(eq + 1);
				const size_t valueBegin = v;
				int depth = 0;
				while (v < end)
				{
					const Token& vt = m_Toks[v];
					if (vt.Kind == TokenKind::Eof)
						break;
					if (depth == 0 && v > valueBegin && !vt.IsComment() && vt.Line > m_Toks[PrevSig(v)].EndLine)
						break;
					if (depth == 0 && vt.Is(";"))
						break;
					if (vt.Is("(") || vt.Is("[") || vt.Is("{"))
						depth++;
					else if (vt.Is(")") || vt.Is("]") || vt.Is("}"))
						depth--;
					v++;
				}
				m_StaticSpans.push_back({ j, nameTok, v });
				info.Statics.push_back(std::string(m_Toks[nameTok].Text));
				return Sig(v);
			}

			// `signal Name(a: T, ...)`: every parameter typed (the payload's
			// types are the point of the declaration).
			size_t ScanSignal(size_t j, size_t end, ClassInfo& info)
			{
				const size_t nameTok = Sig(j + 1);
				const size_t open = Sig(nameTok + 1);
				if (!m_Toks[open].Is("("))
				{
					Error(m_Toks[j], "a signal is: signal Name(param: Type, ...)");
					return Sig(nameTok + 1);
				}
				const size_t close = MatchClose(open);
				ClassSignal signal;
				signal.Name = std::string(m_Toks[nameTok].Text);
				signal.Params = Collapse(SourceText(open + 1, close));
				signal.At = { { m_Toks[nameTok].Line, m_Toks[nameTok].Col }, { m_Toks[nameTok].EndLine, m_Toks[nameTok].EndCol } };
				int depth = 0;
				bool typed = true;
				size_t paramStart = open + 1;
				for (size_t k = open + 1; k <= close; k++)
				{
					const Token& t = m_Toks[k];
					if (t.Is("(") || t.Is("{") || t.Is("[") || t.Is("<"))
						depth++;
					else if ((t.Is(")") || t.Is("}") || t.Is("]") || t.Is(">")) && k != close)
						depth--;
					if ((t.Is(",") && depth == 0) || k == close)
					{
						if (k > paramStart)
						{
							bool colon = false;
							for (size_t m = paramStart; m < k; m++)
								colon = colon || m_Toks[m].Is(":");
							typed = typed && colon;
						}
						paramStart = k + 1;
					}
				}
				if (!typed)
					Error(m_Toks[nameTok], "a signal's parameters declare their types: signal " + signal.Name + "(param: Type)");
				for (const ClassSignal& other : info.Signals)
					if (other.Name == signal.Name)
						Error(m_Toks[nameTok], "signal " + signal.Name + " is declared twice");
				info.Signals.push_back(std::move(signal));
				m_SignalSpans.push_back({ j, Sig(close + 1) });
				(void)end;
				return Sig(close + 1);
			}

			size_t PrevSig(size_t j) const
			{
				while (j > 0)
				{
					j--;
					if (!m_Toks[j].IsComment())
						return j;
				}
				return 0;
			}

			// `abstract function Name(params): Type`: a signature, no body.
			size_t ScanAbstract(size_t abstractTok, size_t end, ClassInfo& info)
			{
				const size_t fn = Sig(abstractTok + 1);
				ClassMethod m;
				m.Abstract = true;
				const size_t nameTok = Sig(fn + 1);
				if (m_Toks[nameTok].Kind != TokenKind::Name)
					return Sig(fn + 1);
				m.Name = std::string(m_Toks[nameTok].Text);
				m.At = { { m_Toks[nameTok].Line, m_Toks[nameTok].Col }, { m_Toks[nameTok].EndLine, m_Toks[nameTok].EndCol } };
				size_t p = Sig(nameTok + 1);
				if (m_Toks[p].Is("<"))
					p = Sig(MatchClose(p) + 1);
				if (!m_Toks[p].Is("("))
					return Sig(nameTok + 1);
				const size_t close = MatchClose(p);
				m.Params = Collapse(SourceText(p + 1, close));
				size_t last = close;
				size_t k = Sig(close + 1);
				if (m_Toks[k].Is(":"))
				{
					const size_t typeEnd = SkipType(k + 1);
					m.ReturnType = Collapse(SourceText(Sig(k + 1), typeEnd));
					last = PrevSig(typeEnd);
					k = Sig(typeEnd);
				}
				if (m_Toks[k].Is("{"))
					Error(m_Toks[nameTok], "an abstract method has no body (a subclass gives it one): abstract function " + m.Name + "()");
				m.BodyBeginLine = m.BodyEndLine = m_Toks[nameTok].Line;
				info.Methods.push_back(std::move(m));
				m_AbstractSpans.push_back({ abstractTok, fn, last });
				(void)end;
				return Sig(last + 1);
			}

			size_t ScanMethod(size_t fn, size_t end, ClassInfo& info)
			{
				ClassMethod m;
				const size_t nameTok = Sig(fn + 1);
				if (m_Toks[nameTok].Kind != TokenKind::Name)
					return Sig(fn + 1);
				m.Name = std::string(m_Toks[nameTok].Text);
				m.At = { { m_Toks[nameTok].Line, m_Toks[nameTok].Col }, { m_Toks[nameTok].EndLine, m_Toks[nameTok].EndCol } };
				size_t p = Sig(nameTok + 1);
				if (m_Toks[p].Is("<"))
					p = Sig(MatchClose(p) + 1);
				if (!m_Toks[p].Is("("))
					return Sig(nameTok + 1);
				const size_t close = MatchClose(p);
				m.Params = Collapse(SourceText(p + 1, close));
				size_t k = Sig(close + 1);
				if (m_Toks[k].Is(":"))
				{
					const size_t typeEnd = SkipType(k + 1);
					m.ReturnType = Collapse(SourceText(Sig(k + 1), typeEnd));
					k = Sig(typeEnd);
				}
				if (!m_Toks[k].Is("{"))
					return Sig(k);
				const size_t bodyEnd = MatchClose(k);
				m.BodyBeginLine = m_Toks[k].Line;
				m.BodyEndLine = m_Toks[bodyEnd].Line;
				m.ReturnsValue = BodyReturnsValue(k, bodyEnd);
				info.Methods.push_back(std::move(m));
				(void)end;
				return Sig(bodyEnd + 1);
			}

			// A `return <expression>` in the body itself (not in a nested function).
			bool BodyReturnsValue(size_t open, size_t close) const
			{
				for (size_t j = open + 1; j < close; j++)
				{
					const Token& t = m_Toks[j];
					if (t.Is("function"))
					{
						// Skip the nested function's body.
						size_t p = j + 1;
						while (p < close && !m_Toks[p].Is("("))
							p++;
						if (p >= close)
							continue;
						const size_t brace = FindHeaderBrace(MatchClose(p) + 1);
						if (brace != npos && brace < close)
							j = MatchClose(brace);
						continue;
					}
					if (t.Is("return"))
					{
						const Token& next = m_Toks[Sig(j + 1)];
						if (!(next.Is("}") || next.Is(";") || next.Kind == TokenKind::Eof))
							return true;
					}
				}
				return false;
			}

			void ClassItem(size_t i)
			{
				for (const AbstractSpan& span : m_AbstractSpans)
					if (span.Begin == i)
					{
						Generate(i, "");
						m_AbstractFunction = span.Function;
						m_AbstractEnd = span.End;
						m_PrevSig = i;
						return;
					}
				for (const SignalSpan& span : m_SignalSpans)
				{
					if (span.Begin != i)
						continue;
					for (size_t k = i; k < span.End; k++)
						if (!m_Toks[k].IsComment())
						{
							m_Repl[k] = "";
							m_Skip[k] = 1;
						}
					m_PrevSig = span.End > 0 ? span.End - 1 : i;
					return;
				}
				for (const StaticSpan& span : m_StaticSpans)
				{
					if (span.Begin != i)
						continue;
					m_Repl[i] = "";
					m_Repl[span.Name] = m_ClassName + "." + std::string(m_Toks[span.Name].Text);
					m_Skip[span.Name] = 1;
					m_StaticEnd = span.End; // the expression's tokens are ordinary code
					m_PrevSig = i;
					return;
				}
				for (const FieldSpan& span : m_FieldSpans)
				{
					if (span.Begin != i)
						continue;
					std::string error;
					std::string lowered;
					if (m_Profile->LowerField)
						lowered = m_Profile->LowerField(span.Field, m_ClassName, error);
					else if (!span.Field.Default.empty())
						lowered = m_ClassName + ".Fields." + span.Field.Name + " = " + span.Field.Default;
					if (!error.empty())
						Error(m_Toks[i], error);
					m_Repl[i] = lowered;
					for (size_t k = i + 1; k < span.End; k++)
						if (!m_Toks[k].IsComment())
						{
							m_Repl[k] = "";
							m_Skip[k] = 1;
						}
					m_PrevSig = span.End > 0 ? span.End - 1 : i;
					return;
				}
				// Not a field the scan understood (it reported why).
				m_Repl[i] = "";
				m_PrevSig = i;
			}

			// ---- operators -----------------------------------------------

			void Operator(size_t i)
			{
				const Token& t = m_Toks[i];
				const std::string_view op = t.Text;
				if (const char* r = OperatorReplacement(op))
				{
					m_Repl[i] = r;
					return;
				}
				if (op == "!")
				{
					const bool glued = t.Begin > 0 && (std::isalnum((unsigned char)m_Src[t.Begin - 1]) || m_Src[t.Begin - 1] == ')');
					m_Repl[i] = glued ? " not " : "not ";
					return;
				}
				if (op == "++" || op == "--")
				{
					if (m_PrevSig == npos || !EndsExpression(m_Toks[m_PrevSig]))
						Error(t, std::string(op) + " is a statement after a variable: x" + std::string(op));
					m_Repl[i] = op == "++" ? " += 1" : " -= 1";
					return;
				}
				if (op == "(" )
				{
					m_Stack.push_back({ Frame::Paren, BlockKind::Plain, i });
					return;
				}
				if (op == "[")
				{
					m_Stack.push_back({ Frame::Bracket, BlockKind::Plain, i });
					return;
				}
				if (op == ")" || op == "]")
				{
					const Frame want = op == ")" ? Frame::Paren : Frame::Bracket;
					if (m_Stack.empty() || m_Stack.back().Kind != want)
					{
						Error(t, "unbalanced '" + std::string(op) + "'");
						return;
					}
					m_Stack.pop_back();
					EndExpressionIfsAbove(m_Stack.size());
					return;
				}
				if (op == "{")
				{
					OpenBrace(i);
					return;
				}
				if (op == "}")
					CloseBrace(i);
			}

			bool IsStatementStart(size_t i) const
			{
				if (m_PrevSig == npos)
					return true;
				const Token& p = m_Toks[m_PrevSig];
				if (p.Is(";") || m_OpenedBlock[m_PrevSig] || m_ClosedBlock[m_PrevSig])
					return true;
				if (m_ExprIfClose[m_PrevSig])
					return false;
				if (p.Line == m_Toks[i].Line)
					return false;
				return EndsExpression(p);
			}

			void OpenBrace(size_t i)
			{
				if (m_BlockAt[i].Set)
				{
					m_Stack.push_back({ Frame::Block, m_BlockAt[i].Kind, i });
					m_Repl[i] = m_BlockAt[i].Opener;
					m_OpenedBlock[i] = 1;
					return;
				}
				if (IsStatementStart(i))
				{
					m_Stack.push_back({ Frame::Block, BlockKind::Plain, i });
					m_Repl[i] = "do";
					m_OpenedBlock[i] = 1;
					return;
				}
				m_Stack.push_back({ Frame::Table, BlockKind::Plain, i });
			}

			void CloseBrace(size_t i)
			{
				const Token& t = m_Toks[i];
				while (!m_Stack.empty() && (m_Stack.back().Kind == Frame::Paren || m_Stack.back().Kind == Frame::Bracket))
				{
					Error(m_Toks[m_Stack.back().Token], "unclosed '" + std::string(m_Toks[m_Stack.back().Token].Text) + "'");
					m_Stack.pop_back();
				}
				if (m_Stack.empty())
				{
					Error(t, "unbalanced '}'");
					return;
				}
				const Open open = m_Stack.back();
				m_Stack.pop_back();
				EndExpressionIfsAbove(m_Stack.size());
				if (open.Kind == Frame::Table)
					return;
				m_ClosedBlock[i] = 1;
				if (open.Kind == Frame::Class)
				{
					if (m_Options.Script)
					{
						// A local the code after it uses; CloseAtEnd returns it when
						// it is the file's last statement.
						m_Repl[i] = "";
						m_LastClassEnd = i;
						return;
					}
					m_Repl[i] = "return " + m_ClassName;
					m_ClassEnded = true;
					return;
				}
				if (open.Block == BlockKind::Try || open.Block == BlockKind::Catch || open.Block == BlockKind::Finally)
				{
					const size_t index = m_TryOf[open.Token];
					const Token& next = m_Toks[Sig(i + 1)];
					const bool more = (open.Block == BlockKind::Try && (next.Is("catch") || next.Is("finally"))) ||
						(open.Block == BlockKind::Catch && next.Is("finally"));
					std::string text = open.Block == BlockKind::Try ? "end)"
						: open.Block == BlockKind::Catch           ? "end, " + TryName(m_Tries[index], "sig") + ") end"
																   : "end";
					if (open.Block == BlockKind::Try && !more)
						Error(m_Toks[m_Tries[index].Token], "try needs a catch or a finally: try { } catch (error) { }");
					// The statement ends its function: the next token closes the function's block.
					size_t after = Sig(i + 1);
					while (m_Toks[after].Is(";"))
						after = Sig(after + 1);
					const bool lastInFunction = m_Toks[after].Is("}") && !m_Stack.empty() && m_Stack.back().Kind == Frame::Block &&
						(m_Stack.back().Block == BlockKind::Function || m_Stack.back().Block == BlockKind::Method);
					if (!more)
						text += TryEnd(m_Tries[index], lastInFunction);
					else
						m_TryClosed[i] = { index, open.Block };
					Generate(i, text);
					return;
				}
				switch (open.Block)
				{
				case BlockKind::If:
					if (m_Toks[Sig(i + 1)].Is("else"))
					{
						m_Repl[i] = "";
						m_ClosedIf[i] = 1;
						return;
					}
					m_Repl[i] = "end";
					return;
				case BlockKind::Repeat:
					if (m_Toks[Sig(i + 1)].Is("while"))
					{
						m_Repl[i] = "until";
						m_ClosedRepeat[i] = 1;
						return;
					}
					Error(t, "do { } needs while (...) after its block");
					m_Repl[i] = "until false";
					return;
				default:
					m_Repl[i] = "end";
					return;
				}
			}

			void CloseAtEnd()
			{
				if (m_LastClassEnd != npos && !m_Options.Repl)
				{
					size_t k = Sig(m_LastClassEnd + 1);
					while (m_Toks[k].Is(";"))
						k = Sig(k + 1);
					if (m_Toks[k].Kind == TokenKind::Eof)
						m_Repl[m_LastClassEnd] = "return " + m_ClassName;
				}
				while (!m_ExprIfs.empty())
					MissingElse();
				m_R.Unfinished = !m_Stack.empty() || std::any_of(m_Toks.begin(), m_Toks.end(), [](const Token& t) { return t.Open; });
				while (!m_Stack.empty())
				{
					const Open open = m_Stack.back();
					m_Stack.pop_back();
					const Token& at = m_Toks[open.Token];
					switch (open.Kind)
					{
					case Frame::Paren: m_Tail += ")"; Error(at, "unclosed '('"); break;
					case Frame::Bracket: m_Tail += "]"; Error(at, "unclosed '['"); break;
					case Frame::Table: m_Tail += "}"; Error(at, "unclosed '{'"); break;
					case Frame::Class: m_Tail += m_Options.Script ? std::string() : " return " + m_ClassName; Error(at, "the class's '{' has no closing '}'"); break;
					case Frame::Block:
						m_Tail += open.Block == BlockKind::Repeat ? " until false" : " end";
						Error(at, "this '{' has no closing '}'");
						break;
					}
				}
			}

			// ---- the emit pass -------------------------------------------

			void Emit()
			{
				std::string& out = m_R.Luau;
				out.reserve(m_Src.size() + m_Src.size() / 8);
				int outCol = 0;
				size_t pos = 0;
				auto append = [&](std::string_view text) {
					for (char c : text)
						outCol = c == '\n' ? 0 : outCol + 1;
					out.append(text);
				};
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Token& t = m_Toks[i];
					append(m_Src.substr(pos, t.Begin - pos));
					pos = t.Begin;
					if (t.Kind == TokenKind::Eof)
						break;
					if (!m_Repl[i])
					{
						append(t.Text);
						pos = t.End;
						continue;
					}
					const std::string& text = *m_Repl[i];
					if (t.Kind == TokenKind::BlockComment)
					{
						// The opener and the closer move; the body is copied.
						const size_t open = text.find('[', 3) + 1; // after --[==[
						m_R.Map.AddAnchor(t.Line, t.Col, outCol);
						append(std::string_view(text).substr(0, open));
						m_R.Map.AddAnchor(t.Line, t.Col + 2, outCol);
						const size_t closeLen = open - 2; // ]==]
						append(std::string_view(text).substr(open, text.size() - open - closeLen));
						m_R.Map.AddAnchor(t.EndLine, std::max(0, t.EndCol - 2), outCol);
						append(std::string_view(text).substr(text.size() - closeLen));
						m_R.Map.AddAnchor(t.EndLine, t.EndCol, outCol);
						pos = t.End;
						continue;
					}
					m_R.Map.AddAnchor(t.Line, t.Col, outCol);
					// A word never runs into the word beside it (`{}` is
					// `do end`, `}print()` is `end print()`).
					auto isWord = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
					if (!text.empty() && isWord(text.front()) && !out.empty() && isWord(out.back()))
						append(" ");
					if (auto gen = m_GeneratedAt.find(i); gen != m_GeneratedAt.end())
						m_R.Generated.push_back({ { t.Line, outCol + (int)gen->second.first }, { t.Line, outCol + (int)(gen->second.first + gen->second.second) } });
					append(text);
					if (!m_After[i].empty())
					{
						m_R.Generated.push_back({ { t.EndLine, outCol }, { t.EndLine, outCol + (int)m_After[i].size() } });
						append(m_After[i]);
					}
					if (!out.empty() && isWord(out.back()) && t.End < m_Src.size() && isWord(m_Src[t.End]))
						append(" ");
					m_R.Map.AddAnchor(t.EndLine, t.EndCol, outCol);
					pos = t.End;
				}
				append(m_Tail);
			}

			std::string_view m_Src;
			TranslateOptions m_Options;
			ClassProfile m_DefaultProfile;
			const ClassProfile* m_Profile = nullptr;
			std::vector<Token> m_Toks;
			std::vector<std::optional<std::string>> m_Repl;
			std::vector<std::string> m_After; // generated text after a token (its replacement)
			std::vector<BlockMark> m_BlockAt;
			std::vector<uint8_t> m_OpenedBlock;
			std::vector<uint8_t> m_ClosedBlock;
			std::vector<uint8_t> m_ClosedIf;
			std::vector<uint8_t> m_ClosedRepeat;
			std::vector<uint8_t> m_ExprIfClose; // an if-expression condition's `)`
			std::vector<uint8_t> m_Skip;
			struct ExprIf
			{
				size_t Depth = 0; // m_Stack's size at the `if`: its `else` is at the same depth
				size_t If = 0;
			};
			std::vector<ExprIf> m_ExprIfs; // if-expressions waiting for their `else`
			std::vector<Open> m_Stack;
			std::vector<FieldSpan> m_FieldSpans;
			struct StaticSpan
			{
				size_t Begin = 0; // `static`
				size_t Name = 0;
				size_t End = 0;   // exclusive
			};
			std::vector<StaticSpan> m_StaticSpans;
			struct SignalSpan
			{
				size_t Begin = 0; // `signal`
				size_t End = 0;   // exclusive
			};
			std::vector<SignalSpan> m_SignalSpans;
			size_t m_StaticEnd = npos;
			std::map<size_t, std::pair<size_t, size_t>> m_GeneratedAt; // token -> (offset, length) in its replacement
			struct AbstractSpan
			{
				size_t Begin = 0;    // `abstract`
				size_t Function = 0; // `function`
				size_t End = 0;      // the signature's last token
			};
			std::vector<AbstractSpan> m_AbstractSpans;
			size_t m_AbstractFunction = npos; // the `function` of the abstract method being read
			size_t m_AbstractEnd = npos;
			bool m_AbstractClassNext = false;
			std::map<std::string, std::set<std::string>> m_Unimplemented; // a class's abstract methods no class has defined yet
			std::vector<TryState> m_Tries;
			std::map<size_t, size_t> m_TryOf; // a try's, catch's or finally's `{` -> its try
			std::map<size_t, std::pair<size_t, BlockKind>> m_TryClosed; // a try's or catch's `}` that a catch / finally follows
			std::string m_ClassName;
			std::string m_ClassBase; // the class being translated's base ("" without one)
			std::map<std::string, std::optional<std::string>> m_CtorParams; // each class's constructor parameters, for its subclasses
			size_t m_ClassEndToken = npos;
			bool m_ClassEnded = false;
			size_t m_LastClassEnd = npos; // script mode: the last class's `}`
			size_t m_PrevSig = npos;
			size_t m_MethodHeaderEnd = npos;
			std::string m_Tail;
			TranslateResult m_R;
		};

	} // namespace

	// ---- SourceMap ----------------------------------------------------------

	void SourceMap::AddAnchor(int line, int sourceCol, int luauCol)
	{
		m_Lines[line].push_back({ sourceCol, luauCol });
	}

	Position SourceMap::ToLuau(Position source) const
	{
		auto it = m_Lines.find(source.Line);
		if (it == m_Lines.end() || it->second.empty())
			return source;
		const std::vector<Anchor>& anchors = it->second;
		// The last anchor at or before the column (the last of equals: past an insertion).
		size_t k = npos;
		for (size_t i = 0; i < anchors.size(); i++)
			if (anchors[i].Source <= source.Col)
				k = i;
		if (k == npos)
			return source;
		const Anchor& a = anchors[k];
		int col = a.Luau + (source.Col - a.Source);
		if (k + 1 < anchors.size())
		{
			const Anchor& b = anchors[k + 1];
			// Inside a replaced token: clamp to its replacement.
			if (b.Source - a.Source != b.Luau - a.Luau)
				col = std::min(col, b.Luau);
		}
		return { source.Line, std::max(0, col) };
	}

	Position SourceMap::FromLuau(Position luau) const
	{
		auto it = m_Lines.find(luau.Line);
		if (it == m_Lines.end() || it->second.empty())
			return luau;
		const std::vector<Anchor>& anchors = it->second;
		size_t k = npos;
		for (size_t i = 0; i < anchors.size(); i++)
			if (anchors[i].Luau <= luau.Col)
				k = i;
		if (k == npos)
			return luau;
		const Anchor& a = anchors[k];
		int col = a.Source + (luau.Col - a.Luau);
		if (k + 1 < anchors.size())
		{
			const Anchor& b = anchors[k + 1];
			if (b.Source - a.Source != b.Luau - a.Luau)
				col = std::min(col, b.Source);
		}
		return { luau.Line, std::max(0, col) };
	}

	bool TranslateResult::HasErrors() const
	{
		return std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const Diagnostic& d) { return !d.Warning; });
	}

	TranslateResult Translate(std::string_view source, const TranslateOptions& options)
	{
		return Translator(source, options).Run();
	}

	const ClassProfile& ScriptProfile()
	{
		static const ClassProfile profile = [] {
			ClassProfile p;
			// The class table: its instances' metatable, callable through the
			// runtime's shared `__cluau_meta` (the cast types the call with
			// Init's parameters), naming its base. `__instance` is a
			// type-only member: the instance type, which a subclass's type
			// starts from. Instances: their fields, with a metatable whose
			// __index is the class table (methods and statics) and which
			// carries the metamethods, so `a + b` and `tostring(a)` check too.
			p.Inheritance = true;
			p.Header = [](const ClassInfo& info, const ClassMembers& members) {
				const bool derived = !info.Base.empty();
				// An Init inherited from another module's class: its
				// parameters are unknown here.
				const std::string ctor = members.CtorParams ? *members.CtorParams : std::string(", ...any");
				ClassHeader h;
				// An abstract class is not callable (to the checker; the runtime
				// refuses too), and the abstract methods are listed for the
				// runtime's check of a subclass.
				std::string abstracts;
				for (const ClassMethod& m : info.Methods)
					if (m.Abstract)
						abstracts += (abstracts.empty() ? "" : ", ") + std::string("\"") + m.Name + "\"";
				std::string meta;
				if (!info.Abstract)
					meta = "__call: (unknown" + ctor + ") -> $Class";
				if (derived)
					meta += (meta.empty() ? "" : ", ") + std::string("__index: typeof(") + info.Base + ")";
				h.TableInit = "setmetatable({ Fields = {}, __name = \"$Class\"" + (derived ? ", __base = " + info.Base : std::string()) +
					(info.Abstract ? ", __abstract = true" : "") + (abstracts.empty() ? "" : ", __abstracts = { " + abstracts + " }") +
					", __instance = (nil :: any) :: $Class }, __cluau_meta :: { " + meta + " })";
				// Instances find their methods and statics in the class table
				// (`__index`: its type, which the methods and statics after
				// this line extend), and their metamethods in their metatable.
				std::string metamethods;
				for (const ClassMembers::Method& m : members.Methods)
					if (m.Name.rfind("__", 0) == 0)
						metamethods += m.Name + ": (self: $Class" + m.ParamTypes + ") -> " + m.ReturnType + ", ";
				// A subclass's instances are its base's and its own.
				h.Types = "export type $Class = " + (derived ? "typeof(" + info.Base + ".__instance) & " : std::string()) +
					"typeof(setmetatable({} :: { " + members.Fields + "}, {} :: { __index: typeof($Class), " + metamethods + "}))";
				return h;
			};
			// A default is evaluated for each instance (a `{}` default is
			// each instance's own table).
			p.LowerField = [](const ClassField& field, const std::string& className, std::string&) {
				if (field.Default.empty())
					return std::string();
				return className + ".Fields." + field.Name + " = function() return " + field.Default + " end";
			};
			return p;
		}();
		return profile;
	}

	const char* Version()
	{
		return "0.22.0";
	}

} // namespace cluau
