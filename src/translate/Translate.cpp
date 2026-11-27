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

		// A host's translation that throws takes Luau's `error` on its first
		// line, before a local named `error` can hide it.
		constexpr std::string_view kRaiseHead = "local __cluau_error = error; ";

		// The functions some syntax is translated to calls of, defined on the
		// first line of a chunk that uses them (in this order): `a ?? b` and
		// `a?.b` where `a` is not a name (it is evaluated once; `b`, unless
		// it is a literal or a name, only when needed), and a generator's
		// body, a coroutine that is handed its typed yield; @std/task for
		// async functions and await, and a table literal's spreads.
		enum Helper : uint8_t
		{
			kHelperNil = 1,
			kHelperChain = 2,
			kHelperGenerator = 4,
			kHelperOr = 8,
			kHelperTask = 16,
			kHelperSpread = 32,
		};
		constexpr std::string_view kHelperText[] = {
			"local function __cluau_nil(value: any, fallback: () -> any): any if value ~= nil then return value end return fallback() end; ",
			"local function __cluau_chain<T, U>(value: T?, f: (T) -> U): U? if value == nil then return nil end return f(value) end; ",
			"local function __cluau_generator<T...>(body: ((T...) -> ()) -> ()): () -> T... return coroutine.wrap(function() body(coroutine.yield) end) :: any end; ",
			"local function __cluau_or(value: any, fallback: any): any if value ~= nil then return value end return fallback end; ",
			"local __cluau_task = require(\"@std/task\"); ",
			"local function __cluau_spread(...: any): any local out = {} for i = 1, select(\"#\", ...) do local part = select(i, ...) "
			"if type(part) ~= \"table\" then error(\"cannot spread \" .. typeof(part) .. \" (a table is spread: { ...items })\", 2) end "
			"local n, len = #out, #part table.move(part, 1, len, n + 1, out) for k, v in next, part do "
			"if type(k) ~= \"number\" or k < 1 or k > len or k % 1 ~= 0 then out[k] = v end end end return out end; ",
		};

		// The expressions translated on their own during one translation (a
		// default's value, the parts a typeof names), by their text and mode:
		// nested ones (`a ?? b ?? c`, `{ ...{ ...t } }`) are translated again
		// inside each enclosing one, which without this doubles the work at
		// each level. Kept from the outermost Translate call to its end.
		struct ValueCache
		{
			int Depth = 0;
			std::map<std::string, std::pair<std::string, uint8_t>> Entries; // text -> (its translation, the helpers it uses)
		};
		thread_local ValueCache t_Values;

		// Interp: an interpolated string's segment (its tokens are an expression).
		enum class Frame : uint8_t { Paren, Bracket, Table, Block, Class, Interp };
		enum class BlockKind : uint8_t { Plain, If, Else, Loop, Function, Repeat, Method, Try, Catch, Finally, Switch };

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
			case TokenKind::InterpEnd:
			case TokenKind::LongString: return true;
			case TokenKind::Op: return t.Text == ")" || t.Text == "]" || t.Text == "}";
			default: return false;
			}
		}

		// Brackets, and an interpolated string around its segments.
		bool OpensGroup(const Token& t) { return t.Is("(") || t.Is("[") || t.Is("{") || t.Kind == TokenKind::InterpBegin; }
		bool ClosesGroup(const Token& t) { return t.Is(")") || t.Is("]") || t.Is("}") || t.Kind == TokenKind::InterpEnd; }

		std::string Trim(std::string_view s)
		{
			size_t a = 0, b = s.size();
			while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
				a++;
			while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
				b--;
			return std::string(s.substr(a, b - a));
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

		std::string ExpandClass(std::string text, const std::string& className)
		{
			ReplaceAll(text, "$Class", className);
			return text;
		}

		class Translator
		{
		public:
			// `typeOnly`: the text is read only for its type, inside a
			// typeof (see TypeValue).
			Translator(std::string_view source, const TranslateOptions& options, bool typeOnly = false)
				: m_Src(source), m_Options(options), m_TypeOnly(typeOnly)
			{
				m_Profile = options.Profile ? options.Profile : &m_DefaultProfile;
				if (m_Options.Repl)
					m_Options.Script = true;
			}

			TranslateResult Run()
			{
				m_Toks = detail::Lex(m_Src, m_R.Diagnostics, true);
				const size_t n = m_Toks.size();
				m_Repl.assign(n, std::nullopt);
				m_After.assign(n, std::string());
				m_Before.assign(n, std::string());
				m_StatementStart.assign(n, 0);
				m_BlockAt.assign(n, BlockMark{});
				m_OpenedBlock.assign(n, 0);
				m_ClosedBlock.assign(n, 0);
				m_ClosedIf.assign(n, 0);
				m_ClosedRepeat.assign(n, 0);
				m_ExprIfClose.assign(n, 0);
				m_TypeEnd.assign(n, 0);
				m_DropGap.assign(n, 0);
				m_ThrowAt.assign(n, 0);
				m_TryToken.assign(n, 0);
				m_Skip.assign(n, 0);
				Classify();
				Chains();
				Coalesces();
				Awaits();
				for (size_t i : m_NilAssigns)
					NilAssign(i);
				Emit();
				FindTryLines();
				LimitDiagnostics();
				return std::move(m_R);
			}

		private:
			// ---- helpers -------------------------------------------------

			// The first diagnostics in the file, and how many more there are
			// (a file of stray brackets would otherwise report every one).
			void LimitDiagnostics()
			{
				constexpr size_t kMaxDiagnostics = 50;
				std::vector<Diagnostic>& all = m_R.Diagnostics;
				if (all.size() <= kMaxDiagnostics)
					return;
				std::stable_sort(all.begin(), all.end(), [](const Diagnostic& a, const Diagnostic& b) {
					return std::tie(a.At.Begin.Line, a.At.Begin.Col) < std::tie(b.At.Begin.Line, b.At.Begin.Col);
				});
				Diagnostic more = all[kMaxDiagnostics];
				more.Warning = std::all_of(all.begin() + kMaxDiagnostics, all.end(), [](const Diagnostic& d) { return d.Warning; });
				more.Message = std::to_string(all.size() - kMaxDiagnostics) + " more " + (more.Warning ? "warnings" : "errors") + " from here on are not shown";
				all.resize(kMaxDiagnostics);
				all.push_back(std::move(more));
			}

			size_t Sig(size_t j) const
			{
				while (j < m_Toks.size() && m_Toks[j].IsComment())
					j++;
				return std::min(j, m_Toks.size() - 1);
			}

			bool IsMember() const
			{
				return m_PrevSig != npos && (m_Toks[m_PrevSig].Is(".") || (m_Toks[m_PrevSig].Is(":") && !m_CaseColon.count(m_PrevSig)) ||
					m_Toks[m_PrevSig].Is("::"));
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
						j = SkipSimpleType(Sig(k + 1)); // the loop takes the rest: a chain of arrows is not nested calls
					else
						return j;
				}
			}

			size_t SkipSimpleType(size_t j) const
			{
				const Token& t = m_Toks[j];
				if (t.Is("(") || t.Is("{"))
					return MatchClose(j) + 1;
				if (t.Is("<"))
				{
					// A generic function type: `<T>(T) -> T`.
					const size_t p = Sig(MatchClose(j) + 1);
					return m_Toks[p].Is("(") ? MatchClose(p) + 1 : j;
				}
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
					if (t.Is("{") && depth == 0)
						return k;
					if (OpensGroup(t))
						depth++;
					else if (ClosesGroup(t))
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

			// The code of tokens [from, to) on one line: comments left out, and
			// one space wherever the source has whitespace or a comment (the
			// tokens themselves, strings included, are copied as they are,
			// except that a string continued on the next line (`\` at a
			// line's end) says its line break with `\n`, and any other line
			// break in a token, which only a mistake puts there, is a space).
			std::string CodeText(size_t from, size_t to) const
			{
				std::string out;
				size_t prevEnd = npos;
				for (size_t k = from; k < to && k < m_Toks.size(); k++)
				{
					const Token& t = m_Toks[k];
					if (t.IsComment() || t.Kind == TokenKind::Eof)
						continue;
					if (prevEnd != npos && t.Begin > prevEnd)
						out += ' ';
					if (t.Line == t.EndLine)
						out.append(t.Text);
					else
						for (size_t c = 0; c < t.Text.size(); c++)
						{
							const char next = c + 1 < t.Text.size() ? t.Text[c + 1] : '\0';
							if (t.Text[c] == '\\' && (next == '\n' || next == '\r'))
							{
								out += "\\n";
								c += next == '\r' && c + 2 < t.Text.size() && t.Text[c + 2] == '\n' ? 2 : 1;
							}
							else if (t.Text[c] == '\\' && next)
							{
								out += t.Text[c];
								out += t.Text[++c];
							}
							else if (t.Text[c] == '\n' || t.Text[c] == '\r')
								out += ' ';
							else
								out += t.Text[c];
						}
					prevEnd = t.End;
				}
				return out;
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

			// Generated text before token `i`, before any already there: what
			// is added later wraps what was added before.
			void AddBefore(size_t i, const std::string& text)
			{
				if (!m_Repl[i])
					m_Repl[i] = std::string(m_Toks[i].Text);
				m_Before[i] = text + m_Before[i];
			}

			// Ends a line that the next line continues.
			static bool AwaitsOperand(const Token& t)
			{
				if (t.Kind == TokenKind::Name)
					return t.Is("and") || t.Is("or") || t.Is("not");
				if (t.Kind != TokenKind::Op)
					return false;
				static constexpr const char* kOps[] = { "=", "+", "-", "*", "/", "%", "^", "..", "==", "!=", "~=", "<", "<=", ">", ">=", "&&",
					"||", "~/", ",", "!", "#", "(", "[", "{", "??", "??=" };
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
				// A statement never starts with a binary operator, so one at a
				// line's start continues the line before (as Luau reads it).
				static constexpr const char* kOps[] = { ".", ":", "..", "+", "-", "*", "/", "%", "^", "==", "!=", "~=", "<", "<=", ">", ">=", "&&", "||",
					"~/", "(", "[", "??" };
				for (const char* op : kOps)
					if (t.Text == op)
						return true;
				return false;
			}

			// The last token of the expression (or list) a statement keyword at
			// `keyword` takes: to the end of its line unless brackets stay open or
			// the line goes on; npos when there is none. With `nextLine`, as Luau
			// reads a `return`, the expression may start on a later line.
			size_t ExpressionEnd(size_t keyword, bool nextLine = false) const
			{
				size_t k = Sig(keyword + 1);
				const Token& first = m_Toks[k];
				if (first.Kind == TokenKind::Eof || first.Is("}") || first.Is(";") ||
					(first.Line > m_Toks[keyword].EndLine && !ContinuesLine(first) && !nextLine))
					return npos;
				int depth = 0;
				size_t last = npos;
				// If-expressions waiting for their else, and their conditions'
				// `)`: a branch may start on the next line.
				int ifs = 0;
				std::set<size_t> conditionClose;
				for (;; k = Sig(k + 1))
				{
					const Token& t = m_Toks[k];
					if (t.Kind == TokenKind::Eof)
						break;
					if (depth == 0 && (t.Is(";") || t.Is("}") || t.Is(")") || t.Is("]")))
						break;
					if (depth == 0 && last != npos && t.Line > m_Toks[last].EndLine && !AwaitsOperand(m_Toks[last]) && !ContinuesLine(t) &&
						!conditionClose.count(last) && !m_Toks[last].Is("else") && !(t.Is("else") && ifs > 0))
						break;
					if (depth == 0 && t.Is("::"))
					{
						// A cast's type, which may end in `>` or `?`, is the expression's.
						const size_t typeEnd = SkipType(k + 1);
						if (typeEnd > Sig(k + 1))
						{
							last = PrevSig(typeEnd);
							k = last;
							continue;
						}
					}
					if (depth == 0 && t.Is("if") && m_Toks[Sig(k + 1)].Is("("))
					{
						if (!(last != npos && m_Toks[last].Is("else")))
							ifs++;
						conditionClose.insert(MatchClose(Sig(k + 1)));
					}
					else if (depth == 0 && t.Is("else") && ifs > 0 && !m_Toks[Sig(k + 1)].Is("if"))
						ifs--;
					if (OpensGroup(t))
						depth++;
					else if (ClosesGroup(t))
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
					if (m_ValueEnd != npos && i >= m_ValueEnd)
						m_ValueEnd = npos;
					if (InClassBody() && m_MethodHeaderEnd == npos && m_ValueEnd == npos && !t.Is("}") && !t.Is("function") && !t.Is(";") &&
						!(t.Is("async") && m_Toks[Sig(i + 1)].Is("function")))
					{
						ClassItem(i);
						continue;
					}
					m_StatementStart[i] = IsStatementStart(i) ? 1 : 0;
					if (InSwitch() && m_StatementStart[i] && !t.Is("case") && !t.Is("default") && !t.Is("}"))
					{
						SwitchState& state = m_Switches[m_Stack.back().Token];
						if (state.Clauses == 0 && !state.Reported)
						{
							Error(t, "a switch holds cases: case value: ... / default: ...");
							state.Reported = true;
						}
					}
					if (t.Kind == TokenKind::Name)
						Name(i);
					else if (t.Kind == TokenKind::Op)
						Operator(i);
					else if (t.Kind == TokenKind::InterpBegin)
						m_Stack.push_back({ Frame::Interp, BlockKind::Plain, i });
					else if (t.Kind == TokenKind::InterpMid || t.Kind == TokenKind::InterpEnd)
						CloseSegment(i);
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
						// A Luau hot comment (--!nonstrict) before the code would
						// change the mode a host enforces: reported. Anywhere
						// else it is an ordinary comment; either way it is
						// defused.
						if (m_PrevSig == npos)
							Error(t, "cluau files have no mode comments (//!...): the host decides the checking mode");
						m_Repl[i] = "-- " + std::string(t.Text.substr(2));
						return;
					}
					// `//[[ x` must not become `--[[ x`, a long comment that
					// would run past the line: a space keeps it a line comment.
					size_t level = 3;
					while (level < t.Text.size() && t.Text[level] == '=')
						level++;
					const bool longBracket = t.Text.size() > 2 && t.Text[2] == '[' && level < t.Text.size() && t.Text[level] == '[';
					m_Repl[i] = (longBracket ? "-- " : "--") + std::string(t.Text.substr(2));
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
				// The closer must first appear where it is put: not in the body,
				// and not made by the body's last characters with it (`t[1]` + `]]`).
				int level = 0;
				for (;; level++)
				{
					const std::string closer = "]" + std::string((size_t)level, '=') + "]";
					if ((std::string(body) + closer).find(closer) == body.size())
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
				else if (w == "async" && m_Toks[Sig(i + 1)].Is("function") && m_Toks[Sig(i + 1)].Line == t.EndLine)
				{
					Generate(i, "");
					m_AsyncNext = true;
				}
				else if (w == "await" && IsAwait(i))
					m_Awaits.push_back(i); // after the chains and ?? in what it waits for
				else if ((w == "let" || w == "local" || w == "const") && (m_Toks[Sig(i + 1)].Is("{") || m_Toks[Sig(i + 1)].Is("[")))
					Destructure(i);
				else if ((w == "let" || w == "local" || w == "const") && m_Options.Repl && m_Stack.empty())
					ReplDeclaration(i);
				else if (w == "let" || w == "local")
				{
					m_Repl[i] = "local";
					MarkDeclarationTypes(i);
				}
				else if (w == "const")
				{
					Const(i);
					MarkDeclarationTypes(i);
				}
				else if (w == "type" && m_Toks[Sig(i + 1)].Kind == TokenKind::Name &&
					(m_Toks[Sig(Sig(i + 1) + 1)].Is("=") || m_Toks[Sig(Sig(i + 1) + 1)].Is("<")))
					TypeAlias(i);
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
				// (`try` and a `{` on the next line: a name alone is no statement,
				// so this is the statement too.)
				else if (w == "try" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Is("{"))
					Try(i);
				else if (w == "enum" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Kind == TokenKind::Name && m_Toks[Sig(Sig(i + 1) + 1)].Is("{"))
					Enum(i);
				else if (w == "catch" && m_PrevSig != npos && m_TryClosed.count(m_PrevSig) && m_TryClosed[m_PrevSig].second == BlockKind::Try)
					Catch(i);
				else if (w == "finally" && m_PrevSig != npos && m_TryClosed.count(m_PrevSig))
					Finally(i);
				else if (w == "switch" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Is("(") && m_Toks[Sig(MatchClose(Sig(i + 1)) + 1)].Is("{"))
					Switch(i);
				else if (w == "case" && InSwitch() && IsStatementStart(i))
					Case(i);
				else if (w == "default" && InSwitch() && IsStatementStart(i) && m_Toks[Sig(i + 1)].Is(":"))
					Default(i);
				else if (w == "return")
					Return(i);
				else if (w == "yield" && IsStatementStart(i) && IsKeywordUse(i) && InGenerator())
					Yield(i);
				else if (w == "yield" && IsStatementStart(i) && m_Toks[Sig(i + 1)].Line != t.Line && InGenerator())
					Error(t, "yield gives a value: yield x");
				else if ((w == "break" || w == "continue") && IsStatementStart(i) && !IsKeywordUseAsName(i))
					Jump(i);
				else if (w == "then" || w == "end" || w == "elseif" || w == "repeat" || w == "until")
					Error(t, "cluau uses braces for blocks: '" + std::string(w) + "' is Luau syntax (see docs/syntax.md)");
			}

			// `await value` in a script: the value (a task) is waited for. Not
			// a name declared, called through a member or assigned to.
			bool IsAwait(size_t i) const
			{
				if (!m_Options.Script || !StartsOperand(i + 1, m_Toks[i].EndLine))
					return false;
				const size_t before = PrevSig(i);
				return before == i || !(m_Toks[before].Is("function") || m_Toks[before].Is("local") || m_Toks[before].Is("let") ||
										   m_Toks[before].Is("const") || m_Toks[before].Is("."));
			}

			// The last token of what an `await` at i waits for: a value and its
			// fields, indexes, calls and optional chains (`await t`, `await
			// load(url)`, `await self.tasks[1]`).
			size_t AwaitOperandEnd(size_t i) const
			{
				size_t k = Sig(i + 1);
				if (m_Toks[k].Is("await") && IsAwait(k))
					return AwaitOperandEnd(k);
				size_t last = OpensGroup(m_Toks[k]) ? MatchGroup(k) : k;
				if (m_Toks[k].Is("function"))
				{
					// A function value: to its body's end.
					size_t p = Sig(k + 1);
					if (!m_Toks[p].Is("("))
						return k;
					const size_t brace = FindHeaderBrace(MatchClose(p) + 1);
					if (brace == npos)
						return k;
					last = MatchClose(brace);
				}
				for (;;)
				{
					size_t n = Sig(last + 1);
					if (IsChainStep(n))
						n++;
					const Token& s = m_Toks[n];
					if ((s.Is(".") || s.Is(":")) && m_Toks[Sig(n + 1)].Kind == TokenKind::Name)
						last = Sig(n + 1);
					else if (s.Is("["))
						last = MatchClose(n);
					else if (s.Is("(") && s.Line == m_Toks[last].EndLine)
						last = MatchClose(n);
					else if ((s.Kind == TokenKind::String || s.Kind == TokenKind::LongString || s.Kind == TokenKind::Interpolated) &&
						s.Line == m_Toks[last].EndLine)
						last = n;
					else if (s.Kind == TokenKind::InterpBegin && s.Line == m_Toks[last].EndLine)
						last = MatchGroup(n);
					else
						return last;
				}
			}

			// `await task`: task.join, which waits for it (in a task, letting
			// the others run) and gives back what its function returned. From
			// the last: an inner one's closing text comes first.
			void Awaits()
			{
				for (size_t n = m_Awaits.size(); n-- > 0;)
				{
					const size_t i = m_Awaits[n];
					if (m_Skip[i])
						continue;
					m_Helpers |= kHelperTask;
					Generate(i, "__cluau_task.join(");
					AddAfter(AwaitOperandEnd(i), ")");
				}
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
				m_ThrowAt[i] = 1;
			}

			// What raises an error as it is: in a script, the runtime's
			// __cluau_throw; for a host, Luau's `error` (with level 0), taken at
			// the top of the chunk (Emit) before a local named `error` (a
			// parameter, `catch (error)`) can hide it.
			std::string Raise() const
			{
				if (m_Options.Script)
					return "__cluau_throw";
				m_RaiseUsed = true;
				return "__cluau_error";
			}

			std::string RaiseCall(const std::string& value) const
			{
				return m_Options.Script ? "__cluau_throw(" + value + ")" : Raise() + "(" + value + ", 0)";
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
				// The string members' union, and `number` for the number members.
				std::string type;
				for (size_t k = 0; k < strings.size(); k++)
					type += (k ? " | " : "") + strings[k];
				if (numbers)
					type += type.empty() ? "number" : " | number";
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
				size_t ReturnAt = npos, BreakAt = npos, ContinueAt = npos; // the first of each (in this try or one inside it)
				bool HasCatch = false, HasFinally = false;
				size_t CatchAt = npos; // the `catch` keyword
				bool TryVarargs = false, CatchVarargs = false; // the block uses `...`: its closure passes them on
			};

			// The type of the `...` of the function around here (`any` when it
			// declares none, or at the top of the chunk).
			std::string VarargType() const
			{
				for (size_t k = m_Stack.size(); k-- > 0;)
				{
					const Open& open = m_Stack[k];
					if (open.Kind == Frame::Block && (open.Block == BlockKind::Function || open.Block == BlockKind::Method))
					{
						auto it = m_VarargType.find(open.Token);
						return it == m_VarargType.end() ? "any" : it->second;
					}
				}
				return "any";
			}

			// The block between `open` and `close` uses its function's `...`
			// as a value (not in a nested function, not as a type pack such
			// as `...number`).
			bool UsesVarargs(size_t open, size_t close) const
			{
				for (size_t j = open + 1; j < close; j++)
				{
					const Token& t = m_Toks[j];
					if (t.Is("function"))
					{
						size_t p = Sig(j + 1);
						while (p < close && !m_Toks[p].Is("(") && !m_Toks[p].Is("{"))
							p = Sig(p + 1);
						if (p < close && m_Toks[p].Is("("))
						{
							const size_t brace = FindHeaderBrace(MatchClose(p) + 1);
							if (brace != npos && brace < close)
								j = MatchClose(brace);
						}
						continue;
					}
					if (!t.Is("..."))
						continue;
					const Token& next = m_Toks[Sig(j + 1)];
					const Token& prev = m_Toks[PrevSig(j)];
					const bool typePack = (next.Kind == TokenKind::Name && next.Begin == t.End) ||
						(prev.Kind == TokenKind::Name && !IsExpressionWord(prev.Text) && prev.End == t.Begin);
					if (!typePack)
						return true;
				}
				return false;
			}

			std::string TryName(const TryState& t, const char* part) const { return "__try" + std::to_string(t.Id) + part; }

			void Try(size_t i)
			{
				if (m_Options.Manifest)
					Error(m_Toks[i], "manifests are declarations: try is not allowed");
				TryState state;
				state.Id = (int)m_Tries.size() + 1;
				state.Token = i;
				m_Tries.push_back(state);
				const TryState& t = m_Tries.back();
				const size_t brace = Sig(i + 1);
				// A block that uses `...` gets the function's: the closure takes
				// them and pcall passes them (CloseBrace).
				m_Tries.back().TryVarargs = UsesVarargs(brace, MatchClose(brace));
				Generate(i, "do local " + TryName(t, "ok") + ": boolean, " + TryName(t, "sig") + ": any, " + TryName(t, "val") +
					": any = pcall(function(" + (t.TryVarargs ? "...: " + VarargType() : std::string()) + "): ...any");
				MarkBlock(brace, BlockKind::Try, "");
				m_TryOf[brace] = m_Tries.size() - 1;
			}

			void Catch(size_t i)
			{
				const size_t index = m_TryClosed[m_PrevSig].first;
				TryState& t = m_Tries[index];
				t.HasCatch = true;
				t.CatchAt = i;
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
				for (size_t k = i; k <= brace; k++)
					m_TryToken[k] = 1;
				t.CatchVarargs = UsesVarargs(brace, MatchClose(brace));
				if (t.CatchVarargs)
					Generate(close, ", ...: " + VarargType() + ")");
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
				m_TryToken[i] = m_TryToken[brace] = 1;
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
				if (InGenerator() && ExpressionEnd(i, true) != npos)
					Error(m_Toks[i], "a generator returns no value: return ends it, and yield gives its values");
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
				m_Tries[(size_t)index].ReturnAt = std::min(m_Tries[(size_t)index].ReturnAt, i);
				const size_t last = ExpressionEnd(i, true);
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
				if (isBreak)
					for (size_t k = m_Stack.size(); k-- > 0;)
					{
						const Open& open = m_Stack[k];
						if (open.Kind != Frame::Block || (open.Block != BlockKind::Switch && open.Block != BlockKind::Loop &&
															 open.Block != BlockKind::Repeat && open.Block != BlockKind::Function &&
															 open.Block != BlockKind::Method))
							continue;
						if (open.Block == BlockKind::Switch)
						{
							Error(m_Toks[i], "a case ends on its own: a switch needs no break (and one cannot leave the loop around it)");
							return;
						}
						break;
					}
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
				TryState& state = m_Tries[(size_t)index];
				(isBreak ? state.Breaks : state.Continues) = true;
				size_t& at = isBreak ? state.BreakAt : state.ContinueAt;
				at = std::min(at, i);
				Generate(i, isBreak ? "return 2" : "return 3");
			}

			// After the try's last block: an error left over is raised again, and a
	// return / break / continue from inside happens here (or travels on, out
	// of an enclosing try). `last`: the statement ends its function, so its
	// return is unconditional (the function returns on every path).
	std::string TryEnd(const TryState& t, bool last)
	{
		const std::string ok = TryName(t, "ok"), sig = TryName(t, "sig"), val = TryName(t, "val");
		std::string out = " if not " + ok + " then " + RaiseCall(sig) + " end";
		bool finally = false, loop = false;
		for (const auto& [wanted, code, word, at] :
			{ std::tuple{ t.Breaks, 2, "break", t.BreakAt }, std::tuple{ t.Continues, 3, "continue", t.ContinueAt } })
		{
			if (!wanted)
				continue;
			const int outer = TryInTheWay(true, finally, loop);
			if (finally)
			{
				// A try inside a finally block: its jump would leave the finally too.
				Error(m_Toks[at], std::string("a finally block runs on its way out: it cannot ") + word);
				continue;
			}
			if (outer >= 0)
			{
				TryState& o = m_Tries[(size_t)outer];
				(code == 2 ? o.Breaks : o.Continues) = true;
				size_t& outerAt = code == 2 ? o.BreakAt : o.ContinueAt;
				outerAt = std::min(outerAt, at);
				out += " if " + sig + " == " + std::to_string(code) + " then return " + std::to_string(code) + " end";
			}
			else
				out += " if " + sig + " == " + std::to_string(code) + " then " + word + " end";
		}
		if (t.Returns)
		{
			const int outer = TryInTheWay(false, finally, loop);
			if (finally)
				Error(m_Toks[t.ReturnAt], "a finally block runs on its way out: it cannot return");
			else if (outer >= 0)
			{
				m_Tries[(size_t)outer].Returns = true;
				m_Tries[(size_t)outer].ReturnAt = std::min(m_Tries[(size_t)outer].ReturnAt, t.ReturnAt);
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
				else if (parenthesised)
				{
					// `if (x) print(x)`: a statement after the condition, not its
					// block (which a `{` further on must not be taken for).
					const Token& after = m_Toks[Sig(MatchClose(c) + 1)];
					if ((after.Kind == TokenKind::Name && !after.Is("and") && !after.Is("or")) || after.Kind == TokenKind::Number ||
						after.Kind == TokenKind::String || after.Kind == TokenKind::LongString || after.Kind == TokenKind::Interpolated ||
						after.Kind == TokenKind::InterpBegin)
					{
						Error(t, std::string(t.Text) + " needs a { } block: " + std::string(t.Text) + " (...) { ... }");
						return;
					}
				}
				const size_t brace = FindHeaderBrace(c);
				if (brace == npos)
				{
					Error(t, std::string(t.Text) + " needs a { } block");
					return;
				}
				if (parenthesised && m_Toks[c].Is("("))
				{
					// `if (x) (f)()` and a table on a later line: a statement ends
					// between the condition and that `{`, which is not the block.
					size_t prev = MatchClose(c);
					int depth = 0;
					for (size_t k = Sig(prev + 1); k < brace; k = Sig(k + 1))
					{
						const Token& next = m_Toks[k];
						if (depth == 0 && next.Line > m_Toks[prev].EndLine && !AwaitsOperand(m_Toks[prev]) && !ContinuesLine(next))
						{
							Error(t, std::string(t.Text) + " needs a { } block: " + std::string(t.Text) + " (...) { ... }");
							return;
						}
						if (OpensGroup(next))
							depth++;
						else if (ClosesGroup(next))
							depth--;
						prev = k;
					}
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
				if (const size_t let = Sig(p + 1); m_Toks[let].Is("let") || m_Toks[let].Is("local"))
					Blank(let);
				// A pattern for a loop variable, `for (_, { name, size } in
				// items)`: the loop's variable is a local of its own, and the
				// pattern's locals are read from it as the body starts.
				std::string locals;
				int position = 0;
				for (size_t k = Sig(p + 1); k < q;)
				{
					const Token& v = m_Toks[k];
					if (v.Is("in") || v.Is("="))
					{
						if (!locals.empty() && v.Is("="))
							Error(v, "a numeric loop counts with a name: for (i = 1, n)");
						break;
					}
					if (v.Is(","))
					{
						k = Sig(k + 1);
						continue;
					}
					position++;
					if (!v.Is("{") && !v.Is("["))
					{
						k = Sig(k + 1);
						continue;
					}
					std::vector<PatternLeaf> leaves;
					std::vector<size_t> syntax;
					if (!ParsePattern(k, "", leaves, syntax))
						return;
					const size_t end = MatchClose(k);
					for (size_t s : syntax)
						Blank(s);
					for (const PatternLeaf& leaf : leaves)
						Blank(leaf.Name);
					for (size_t c = k; c <= end; c++)
						if (m_Toks[c].Is(","))
							Blank(c);
					const std::string name = "__cluau_v" + std::to_string(position);
					Generate(k, name);
					locals += PatternLocals(leaves, name);
					k = Sig(end + 1);
				}
				if (locals.empty())
					TypeClassLoop(p, q);
				MarkBlock(brace, BlockKind::Loop, "do" + locals);
				if (!locals.empty())
					m_GeneratedAt[brace] = { 2, locals.size() };
			}

			// `for (name, value in Class)` over a class this file declared
			// earlier: the runtime's __iter gives the class's own members, by
			// name. Luau's checker types the variables of a loop over a value
			// with __iter as any (it does not call the metamethod's type), so
			// the variables are annotated with what __iter gives. Not when the
			// file declares something else by the class's name (a local, a
			// parameter or a loop variable could be what the loop goes over).
			void TypeClassLoop(size_t open, size_t close)
			{
				if (m_Profile != &ScriptProfile())
					return;
				std::vector<size_t> names;
				size_t k = Sig(open + 1);
				while (m_Toks[k].Kind == TokenKind::Name && !IsExpressionWord(m_Toks[k].Text))
				{
					names.push_back(k);
					k = Sig(k + 1);
					if (!m_Toks[k].Is(","))
						break;
					k = Sig(k + 1);
				}
				if (names.empty() || names.size() > 2 || !m_Toks[k].Is("in"))
					return;
				const size_t value = Sig(k + 1);
				if (m_Toks[value].Kind != TokenKind::Name || Sig(value + 1) != close)
					return;
				const std::string_view name = m_Toks[value].Text;
				if (std::none_of(m_R.Classes.begin(), m_R.Classes.end(), [&](const ClassInfo& c) { return c.Name == name; }))
					return;
				for (size_t j = 0; j < m_Toks.size(); j++)
				{
					if (m_Toks[j].Kind != TokenKind::Name || m_Toks[j].Text != name)
						continue;
					const Token& before = m_Toks[PrevSig(j)];
					const Token& after = m_Toks[Sig(j + 1)];
					if (before.Is("let") || before.Is("const") || before.Is("local") || before.Is("function") ||
						((before.Is("(") || before.Is(",")) && (after.Is(":") || after.Is("in"))))
						return;
				}
				AddAfter(names[0], ": string");
				if (names.size() > 1)
					AddAfter(names[1], ": unknown");
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
				if (m_ExprIfClose[m_PrevSig] || m_ThrowAt[m_PrevSig])
					return true; // if (a) if (b) 1 else 2 else 3; throw if (c) a else b
				const Token& p = m_Toks[m_PrevSig];
				if (p.Kind == TokenKind::InterpBegin || p.Kind == TokenKind::InterpMid)
					return true; // a segment's start
				if (p.Is(";") && !m_Stack.empty() && m_Stack.back().Kind == Frame::Table)
					return true; // a table's `;` separates its fields
				if (p.Kind == TokenKind::Op)
					return !(p.Is(";") || EndsStatement(m_PrevSig));
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

			// A name a pattern can declare: not a keyword or a literal.
			bool IsPatternName(size_t k) const
			{
				const Token& t = m_Toks[k];
				return t.Kind == TokenKind::Name && !IsExpressionWord(t.Text) && !t.Is("nil") && !t.Is("true") && !t.Is("false") && !t.Is("end");
			}

			// One local a destructuring pattern declares.
			struct PatternLeaf
			{
				size_t Name = npos;       // the local's name
				std::string Read;         // how it is read from the value: `.a`, `[2].b`
				size_t Default = npos;    // its default's first token
				size_t DefaultEnd = npos; // after the default's last token
				size_t RestFrom = 0;      // a list's rest: the first item it takes (`Read` is the list)
			};

			// The pattern `{ ... }` or `[ ... ]` at `open`, read from `path`:
			// its locals in order, and the tokens that are not Luau's (its
			// brackets, renamed fields, defaults; the commas between locals
			// stay). False after reporting what is wrong.
			bool ParsePattern(size_t open, const std::string& path, std::vector<PatternLeaf>& leaves, std::vector<size_t>& syntax)
			{
				const bool fields = m_Toks[open].Is("{");
				const size_t close = MatchClose(open);
				const char* usage = fields ? "a { } pattern takes fields: { name, size: n, pos: { x, y }, label = \"none\" }"
										   : "a [ ] pattern takes items: [first, [a, b], third = 0, ...rest]";
				if (close >= m_Toks.size() || !m_Toks[close].Is(fields ? "}" : "]"))
				{
					Error(m_Toks[open], usage);
					return false;
				}
				syntax.push_back(open);
				syntax.push_back(close);
				size_t index = 0;
				const size_t before = leaves.size();
				for (size_t k = Sig(open + 1); k < close;)
				{
					index++;
					PatternLeaf leaf;
					if (m_Toks[k].Is("..."))
					{
						const size_t name = Sig(k + 1);
						if (fields)
						{
							Error(m_Toks[k], "a { } pattern takes fields by name; the rest of a list is taken in a [ ] pattern: [first, ...rest]");
							return false;
						}
						if (!IsPatternName(name))
						{
							Error(m_Toks[k], "a rest element is a name: [first, ...rest]");
							return false;
						}
						if (Sig(name + 1) != close)
						{
							Error(m_Toks[k], "a rest element is the pattern's last: [first, ...rest]");
							return false;
						}
						syntax.push_back(k);
						leaf.Name = name;
						leaf.Read = path;
						leaf.RestFrom = index;
						leaves.push_back(leaf);
						break;
					}
					std::string read;
					size_t target = k;
					if (fields)
					{
						if (!IsPatternName(k))
						{
							Error(m_Toks[k], usage);
							return false;
						}
						read = path + "." + std::string(m_Toks[k].Text);
						const size_t colon = Sig(k + 1);
						if (m_Toks[colon].Is(":"))
						{
							syntax.push_back(k);
							syntax.push_back(colon);
							target = Sig(colon + 1);
						}
					}
					else
						read = path + "[" + std::to_string(index) + "]";
					if (m_Toks[target].Is("{") || m_Toks[target].Is("["))
					{
						if (!ParsePattern(target, read, leaves, syntax))
							return false;
						k = Sig(MatchClose(target) + 1);
						if (m_Toks[k].Is("="))
						{
							Error(m_Toks[k], "a nested pattern has no default: give its fields theirs");
							return false;
						}
					}
					else
					{
						if (!IsPatternName(target))
						{
							Error(m_Toks[target], usage);
							return false;
						}
						leaf.Name = target;
						leaf.Read = read;
						k = Sig(target + 1);
						if (m_Toks[k].Is("="))
						{
							size_t end = Sig(k + 1);
							while (end < close && !m_Toks[end].Is(","))
								end = OpensGroup(m_Toks[end]) ? Sig(MatchGroup(end) + 1) : Sig(end + 1);
							if (Sig(k + 1) >= end)
							{
								Error(m_Toks[k], "a default needs a value: name = value");
								return false;
							}
							for (size_t d = k; d < end; d++)
								syntax.push_back(d);
							leaf.Default = Sig(k + 1);
							leaf.DefaultEnd = end;
							k = end;
						}
						leaves.push_back(leaf);
					}
					if (k < close)
					{
						if (!m_Toks[k].Is(","))
						{
							Error(m_Toks[k], usage);
							return false;
						}
						if (Sig(k + 1) == close)
							syntax.push_back(k); // a trailing comma
						k = Sig(k + 1);
					}
				}
				if (leaves.size() == before)
				{
					Error(m_Toks[open], usage);
					return false;
				}
				return true;
			}

			// A pattern's locals as statements, read from the local `base`. A
			// default is evaluated only when its value is nil, and may use the
			// locals before it.
			std::string PatternLocals(const std::vector<PatternLeaf>& leaves, const std::string& base) const
			{
				std::string out;
				for (size_t n = 0; n < leaves.size(); n++)
				{
					const PatternLeaf& leaf = leaves[n];
					const std::string name(m_Toks[leaf.Name].Text);
					const std::string read = base + leaf.Read;
					if (leaf.RestFrom)
						out += " local " + name + " = table.move(" + read + ", " + std::to_string(leaf.RestFrom) + ", #" + read + ", 1, {});";
					else if (leaf.Default != npos)
					{
						const std::string value = "__cluau_d" + std::to_string(n + 1);
						out += " local " + value + " = " + read + "; local " + name + " = if " + value + " ~= nil then " + value + " else " +
							TranslateValue(leaf.Default, leaf.DefaultEnd, true) + ";";
					}
					else
						out += " local " + name + " = " + read + ";";
				}
				return out;
			}

			// `const { a, b } = value` and `const [first, second] = list`: a
			// local for each field (by its name) or item (in order), through
			// renames (`{ name: n }`), nested patterns, defaults and a list's
			// rest. Without defaults or a rest, a name (or names joined by
			// dots) is read for each, `local a, b = t.a, t.b`; anything else
			// is evaluated once, by a function given it.
			void Destructure(size_t i)
			{
				const Token& t = m_Toks[i];
				const size_t open = Sig(i + 1);
				std::vector<PatternLeaf> leaves;
				std::vector<size_t> syntax;
				if (!ParsePattern(open, "", leaves, syntax))
					return;
				const size_t close = MatchClose(open);
				const size_t eq = Sig(close + 1);
				if (!m_Toks[eq].Is("="))
				{
					Error(t, "destructuring needs its value after =: const { a, b } = value");
					return;
				}
				const size_t last = ExpressionEnd(eq);
				if (last == npos)
				{
					Error(m_Toks[eq], "destructuring takes one value after =");
					return;
				}
				for (size_t k = Sig(eq + 1), depth = 0; k <= last && m_Toks[k].Kind != TokenKind::Eof; k = Sig(k + 1))
				{
					if (OpensGroup(m_Toks[k]))
						depth++;
					else if (ClosesGroup(m_Toks[k]))
						depth--;
					else if (depth == 0 && m_Toks[k].Is(","))
					{
						Error(m_Toks[k], "destructuring takes one value after =");
						return;
					}
				}
				m_Repl[i] = m_Options.Repl && m_Stack.empty() ? "" : "local";
				for (size_t k : syntax)
					Blank(k);
				if (t.Is("const"))
					for (const PatternLeaf& leaf : leaves)
					{
						const Token& n = m_Toks[leaf.Name];
						m_R.ConstNames.push_back({ { n.Line, n.Col }, { n.EndLine, n.EndCol } });
					}
				const bool plain = std::all_of(leaves.begin(), leaves.end(), [](const PatternLeaf& l) { return l.Default == npos && !l.RestFrom; });
				const size_t first = Sig(eq + 1);
				if (plain && IsPath(first, last + 1))
				{
					const std::string value = CodeText(first, last + 1);
					std::string rest = leaves[0].Read;
					for (size_t n = 1; n < leaves.size(); n++)
						rest += ", " + value + leaves[n].Read;
					AddAfter(last, rest);
					return;
				}
				if (HasVarargs(first, last))
				{
					Error(m_Toks[eq], "destructuring ... needs it in a local first: local list = { ... }");
					return;
				}
				std::string body;
				if (plain)
				{
					body = " return ";
					for (size_t n = 0; n < leaves.size(); n++)
						body += (n ? ", __cluau_t" : "__cluau_t") + leaves[n].Read;
				}
				else
				{
					body = PatternLocals(leaves, "__cluau_t") + " return ";
					for (size_t n = 0; n < leaves.size(); n++)
						body += (n ? ", " : "") + std::string(m_Toks[leaves[n].Name].Text);
				}
				Generate(eq, "= (function(__cluau_t: typeof(" + TypeValue(first, last + 1) + "))" + body + " end)(");
				AddAfter(last, ")");
			}

			// The switch block the statement is directly in, if any.
			bool InSwitch() const { return !m_Stack.empty() && m_Stack.back().Kind == Frame::Block && m_Stack.back().Block == BlockKind::Switch; }

			// `switch (value) { case a, b: ... default: ... }`: the value is
			// evaluated once and compared with ==; the first case that matches
			// runs (and no other), default when none does.
			void Switch(size_t i)
			{
				const size_t open = Sig(i + 1);
				const size_t close = MatchClose(open);
				const size_t brace = Sig(close + 1);
				SwitchState state;
				state.Name = "__cluau_switch" + std::to_string(m_Switches.size());
				// A name (or names joined by dots) is compared where it is, so the
				// cases narrow its type.
				state.Subject = IsPath(Sig(open + 1), close) ? CodeText(Sig(open + 1), close) : state.Name;
				Generate(i, "do local " + state.Name + " =");
				MarkBlock(brace, BlockKind::Switch, "");
				m_Switches[brace] = state;
			}

			void Case(size_t i)
			{
				SwitchState& state = m_Switches[m_Stack.back().Token];
				if (state.Default)
					Error(m_Toks[i], "default is a switch's last clause");
				std::vector<size_t> commas;
				size_t colon = npos;
				int depth = 0;
				for (size_t k = Sig(i + 1); m_Toks[k].Kind != TokenKind::Eof; k = Sig(k + 1))
				{
					const Token& t = m_Toks[k];
					if (depth == 0 && (t.Is(":") || t.Is("}")))
					{
						colon = t.Is(":") ? k : npos;
						break;
					}
					if (OpensGroup(t))
						depth++;
					else if (ClosesGroup(t))
						depth--;
					else if (depth == 0 && t.Is(","))
						commas.push_back(k);
				}
				if (colon == npos || colon == Sig(i + 1))
				{
					Error(m_Toks[i], "a case is values and a colon: case 1, 2: (a method call in one is parenthesised)");
					return;
				}
				const std::string compare = state.Subject + " == (";
				Generate(i, (state.Clauses == 0 ? "if " : "elseif ") + compare);
				for (size_t comma : commas)
					Generate(comma, ") or " + compare);
				Generate(colon, ") then");
				m_CaseColon.insert(colon);
				m_OpenedBlock[colon] = 1; // the case's statements follow
				state.Clauses++;
			}

			void Default(size_t i)
			{
				SwitchState& state = m_Switches[m_Stack.back().Token];
				if (state.Default)
					Error(m_Toks[i], "a switch has one default");
				const size_t colon = Sig(i + 1);
				Generate(i, state.Clauses == 0 ? "if true then" : "else");
				Generate(colon, "");
				m_CaseColon.insert(colon);
				m_OpenedBlock[colon] = 1;
				state.Default = true;
				state.Clauses++;
			}

			// A parameter: `name`, `name: Type`, `name = default`,
			// `name: Type = default`, or `...` / `...: T`.
			struct Param
			{
				size_t Begin = npos;     // its first token
				size_t End = npos;       // after its last token (exclusive)
				size_t Name = npos;      // npos for `...`
				size_t TypeBegin = npos; // the annotation's first token
				size_t TypeEnd = npos;   // after the annotation (exclusive)
				size_t Eq = npos;        // the default's `=`
			};

			std::vector<Param> ParseParams(size_t open, size_t close) const
			{
				std::vector<Param> params;
				for (size_t k = Sig(open + 1); k < close;)
				{
					Param p;
					p.Begin = k;
					if (m_Toks[k].Kind == TokenKind::Name)
						p.Name = k;
					k = Sig(k + 1);
					if (m_Toks[k].Is(":"))
					{
						p.TypeBegin = Sig(k + 1);
						p.TypeEnd = SkipType(k + 1);
						k = Sig(p.TypeEnd);
					}
					if (m_Toks[k].Is("="))
						p.Eq = k;
					// To the `,` or the list's end.
					while (k < close && !m_Toks[k].Is(","))
						k = OpensGroup(m_Toks[k]) ? Sig(MatchGroup(k) + 1) : Sig(k + 1);
					p.End = std::min(k, close);
					params.push_back(p);
					if (k < close)
						k = Sig(k + 1);
				}
				return params;
			}

			// `T` made optional: `T?`, `(A | B)?`; one that is already stays.
			static std::string Optional(const std::string& type)
			{
				if (!type.empty() && type.back() == '?')
					return type;
				if (type.find_first_of("|&") != std::string::npos || type.find("->") != std::string::npos)
					return "(" + type + ")?";
				return type + "?";
			}

			// The type a literal default gives an unannotated parameter ("" for
			// any other value).
			std::string LiteralType(size_t from, size_t to) const
			{
				size_t k = Sig(from);
				if (m_Toks[k].Is("-") && m_Toks[Sig(k + 1)].Kind == TokenKind::Number)
					k = Sig(k + 1); // `-1`
				if (Sig(k + 1) < to)
					return "";
				const Token& t = m_Toks[k];
				if (t.Kind == TokenKind::Number)
					return "number";
				if (t.Kind == TokenKind::String || t.Kind == TokenKind::LongString || t.Kind == TokenKind::Interpolated)
					return "string";
				if (t.Is("true") || t.Is("false"))
					return "boolean";
				return "";
			}

			// A parameter list as Luau has it (a class's method types): a
			// parameter with a default is optional to its callers.
			std::string ParamsText(size_t open, size_t close) const
			{
				std::string out;
				for (const Param& p : ParseParams(open, close))
				{
					if (!out.empty())
						out += ", ";
					if (const std::string rest = RestParamType(p); m_Toks[p.Begin].Is("...") && !rest.empty())
					{
						out += rest == "?" ? "..." : "...: " + rest;
						continue;
					}
					if (p.Eq == npos || p.Name == npos)
					{
						out += CodeText(p.Begin, p.End);
						continue;
					}
					std::string type = p.TypeBegin != npos ? CodeText(p.TypeBegin, p.TypeEnd) : LiteralType(Sig(p.Eq + 1), p.End);
					out += std::string(m_Toks[p.Name].Text) + (type.empty() ? "" : ": " + Optional(type));
				}
				return out;
			}

			// A rest parameter's (`...items: T`) item type: "?" without an
			// annotation, "" when `p` is no rest parameter.
			std::string RestParamType(const Param& p) const
			{
				const size_t name = Sig(p.Begin + 1);
				if (!m_Toks[p.Begin].Is("...") || !IsPatternName(name) || m_Toks[name].Begin != m_Toks[p.Begin].End)
					return "";
				const size_t colon = Sig(name + 1);
				if (!m_Toks[colon].Is(":"))
					return "?";
				return CodeText(Sig(colon + 1), SkipType(colon + 1));
			}

			// `function f(first, ...rest: T)`: Luau's `...: T`, and the local
			// `rest`, a list of them, as the body starts.
			std::string RestParam(size_t open, size_t close)
			{
				const std::vector<Param> params = ParseParams(open, close);
				for (size_t n = 0; n < params.size(); n++)
				{
					const Param& p = params[n];
					const std::string type = RestParamType(p);
					if (type.empty())
						continue;
					const size_t name = Sig(p.Begin + 1);
					m_Repl[p.Begin] = "...";
					m_Skip[p.Begin] = 1;
					Blank(name);
					if (n + 1 != params.size())
						Error(m_Toks[p.Begin], "a rest parameter is the last: function f(first, ...rest)");
					if (const size_t eq = Sig(name + 1); m_Toks[eq].Is("="))
						Error(m_Toks[eq], "a rest parameter has no default (it is a list, empty when none are given)");
					return " local " + std::string(m_Toks[name].Text) + (type == "?" ? "" : ": { " + type + " }") + " = { ... };";
				}
				return "";
			}

			// An async function's declared return type as the type of the task
			// it returns: `string` -> `Task<string>`, `(A, B)` -> `Task<(A, B)>`.
			std::string TaskType(size_t from, size_t to) const { return "__cluau_task.Task<" + CodeText(from, to) + ">"; }

			// The parameters with defaults in [open, close): optional in the
			// signature, and given their default at the start of the body
			// `brace` opens (npos: a signature alone).
			void Defaults(size_t open, size_t close, size_t brace)
			{
				std::string start;
				for (const Param& p : ParseParams(open, close))
				{
					if (p.Eq == npos)
						continue;
					const size_t first = Sig(p.Eq + 1);
					if (p.Name == npos || first >= p.End)
					{
						Error(m_Toks[p.Eq], p.Name == npos ? "... has no default" : "a default needs a value: name = value");
						continue;
					}
					const size_t last = PrevSig(p.End);
					if (m_Toks[first].Line != m_Toks[last].EndLine)
						Error(m_Toks[p.Eq], "a parameter's default is written on one line");
					const std::string value = TranslateValue(first, p.End, true);
					for (size_t k = p.Eq; k < p.End; k++)
						Blank(k);
					std::string type;
					if (p.TypeBegin != npos)
					{
						type = CodeText(p.TypeBegin, p.TypeEnd);
						const std::string optional = Optional(type);
						const size_t typeLast = PrevSig(p.TypeEnd);
						if (optional != type && optional.front() == '(')
						{
							AddBefore(p.TypeBegin, "(");
							AddAfter(typeLast, ")?");
						}
						else if (optional != type)
							AddAfter(typeLast, "?");
					}
					else if (!(type = LiteralType(first, p.End)).empty())
						AddAfter(p.Name, ": " + type + "?");
					const std::string name(m_Toks[p.Name].Text);
					start += " local " + name + (type.empty() ? "" : ": " + type) + " = if " + name + " == nil then " + value + " else " + name + ";";
				}
				if (!start.empty() && brace != npos)
					AddAfter(brace, start);
			}

			// A generator's declared type (what it yields, `T` or `(A, B)`) as
			// the function's: an iterator, `() -> T?`, `() -> (A?, B)`, whose
			// first value is nil once it is done.
			std::string GeneratorType(size_t from, size_t to) const
			{
				const size_t first = Sig(from);
				if (m_Toks[first].Is("(") && Sig(MatchClose(first) + 1) >= to)
				{
					const size_t close = MatchClose(first);
					std::vector<Param> items = ParseParams(first, close);
					if (items.empty())
						return "() -> ()";
					std::string out = "() -> (" + Optional(CodeText(items[0].Begin, items[0].End));
					for (size_t n = 1; n < items.size(); n++)
						out += ", " + CodeText(items[n].Begin, items[n].End);
					return out + ")";
				}
				return "() -> " + Optional(CodeText(first, to));
			}

			// Inside a generator's body (not a function inside it).
			bool InGenerator() const
			{
				for (size_t k = m_Stack.size(); k-- > 0;)
				{
					const Open& open = m_Stack[k];
					if (open.Kind == Frame::Class)
						return false;
					if (open.Kind == Frame::Block && (open.Block == BlockKind::Function || open.Block == BlockKind::Method))
						return m_GeneratorBrace.count(open.Token) > 0;
				}
				return false;
			}

			// `yield value(s)` in a generator: hands them to the loop.
			void Yield(size_t i)
			{
				const size_t last = ExpressionEnd(i);
				if (last == npos)
				{
					Error(m_Toks[i], "yield gives a value: yield x");
					return;
				}
				Generate(i, "__cluau_yield(");
				AddAfter(last, ")");
			}

			void Function(size_t i)
			{
				const Token& t = m_Toks[i];
				if (m_Options.Manifest)
					Error(t, "manifests are declarations: function definitions are not allowed");
				// A function value in a static's or a field's expression is an
				// ordinary function, not a method.
				const bool method = InClassBody() && m_ValueEnd == npos;
				size_t j = Sig(i + 1);
				const bool async = m_AsyncNext;
				m_AsyncNext = false;
				// `function*`: a generator.
				const bool generator = m_Toks[j].Is("*");
				if (async && generator)
					Error(t, "a generator is not async: its body runs as its loop asks for values");
				if (generator)
				{
					Generate(j, "");
					m_Skip[j] = 1;
					j = Sig(j + 1);
				}
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
				const size_t paramsOpen = j;
				if (method)
					m_Repl[j] = "(self: " + m_ClassName + (Sig(j + 1) == close ? "" : ", ");
				size_t k = Sig(close + 1);
				std::string yields; // a generator's yield's type
				std::string returns; // an async function's declared return type
				if (m_Toks[k].Is(":"))
				{
					const size_t typeBegin = Sig(k + 1);
					const size_t typeEnd = SkipType(k + 1);
					if (async && !generator && typeEnd > typeBegin)
					{
						// It returns the task its body runs as.
						returns = CodeText(typeBegin, typeEnd);
						Generate(typeBegin, TaskType(typeBegin, typeEnd));
						m_Skip[typeBegin] = 1;
						for (size_t t = typeBegin + 1; t < typeEnd; t++)
							Blank(t);
						m_TypeEnd[typeBegin] = 1;
					}
					if (generator && typeEnd > typeBegin)
					{
						const bool pack = m_Toks[typeBegin].Is("(") && Sig(MatchClose(typeBegin) + 1) >= typeEnd;
						yields = (pack ? CodeText(typeBegin, typeEnd) : "(" + CodeText(typeBegin, typeEnd) + ")") + " -> ()";
						// What it yields becomes what it returns: an iterator.
						Generate(typeBegin, GeneratorType(typeBegin, typeEnd));
						m_Skip[typeBegin] = 1;
						for (size_t t = typeBegin + 1; t < typeEnd; t++)
							Blank(t);
						m_TypeEnd[typeBegin] = 1;
					}
					k = Sig(typeEnd);
				}
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
					Defaults(paramsOpen, close, npos);
					RestParam(paramsOpen, close);
					if (async)
						Error(t, "an abstract method is not async: its subclasses' are");
					if (generator)
						Error(t, "an abstract method is not a generator: its subclasses' are");
					return;
				}
				if (!m_Toks[k].Is("{"))
				{
					Error(m_Toks[k], "a function body is a { } block");
					return;
				}
				const std::string rest = RestParam(paramsOpen, close);
				if (generator)
				{
					// The body runs as a coroutine, its yields typed by what the
					// generator declares it yields.
					const std::string opener = rest + " return __cluau_generator(function(__cluau_yield" + (yields.empty() ? "" : ": " + yields) + ")";
					MarkBlock(k, method ? BlockKind::Method : BlockKind::Function, opener);
					m_GeneratedAt[k] = { 0, opener.size() };
					m_GeneratorBrace.insert(k);
					m_Helpers |= kHelperGenerator;
				}
				else if (async && !m_Options.Script)
				{
					Error(t, "async functions run as @std/task tasks, which only scripts have");
					MarkBlock(k, method ? BlockKind::Method : BlockKind::Function, rest);
				}
				else if (async)
				{
					// The body runs as a task, started now: the function returns it, and
					// an error in it is raised where the task is awaited.
					// The function's `...` is passed on when the body uses it.
					const bool varargs = UsesVarargs(k, MatchClose(k));
					std::string varargType;
					for (size_t p = Sig(paramsOpen + 1); p < close; p = Sig(p + 1))
						if (m_Toks[p].Is("...") && m_Toks[Sig(p + 1)].Is(":"))
							varargType = CodeText(Sig(Sig(p + 1) + 1), SkipType(Sig(p + 1) + 1));
					const std::string opener = rest + " return __cluau_task.async(function(" +
						(varargs ? "..." + (varargType.empty() ? "" : ": " + varargType) : "") + ")" + (returns.empty() ? "" : ": " + returns);
					MarkBlock(k, method ? BlockKind::Method : BlockKind::Function, opener);
					m_GeneratedAt[k] = { 0, opener.size() };
					m_AsyncBrace[k] = varargs ? "end, ...) end" : "end) end";
					m_Helpers |= kHelperTask;
					// Without a declared type, a body that returns nothing gives a
					// task of nothing; one that returns values, a task typed by them.
					if (returns.empty() && !m_Toks[Sig(close + 1)].Is(":") && !BodyReturnsValue(k, MatchClose(k)))
						AddAfter(close, ": __cluau_task.Task<()>");
				}
				else
				{
					MarkBlock(k, method ? BlockKind::Method : BlockKind::Function, rest);
					if (!rest.empty())
						m_GeneratedAt[k] = { 0, rest.size() };
				}
				Defaults(paramsOpen, close, k);
				// Its `...: T`, for a try inside that passes them on.
				for (size_t p = Sig(j + 1); p < close; p = Sig(p + 1))
					if (m_Toks[p].Is("...") && m_Toks[Sig(p + 1)].Is(":"))
						m_VarargType[k] = CodeText(Sig(Sig(p + 1) + 1), SkipType(Sig(p + 1) + 1));
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
				MarkDeclarationTypes(i);
				size_t j = Sig(i + 1);
				if (m_Toks[j].Is("function"))
				{
					m_Repl[i] = "";
					return;
				}
				// The annotations' tokens, which the assignment leaves out.
				std::vector<std::pair<size_t, size_t>> types;
				size_t last = npos;
				while (m_Toks[j].Kind == TokenKind::Name)
				{
					last = j;
					j = Sig(j + 1);
					if (m_Toks[j].Is(":"))
					{
						const size_t typeEnd = SkipType(j + 1);
						// A type that does not end (an unclosed bracket) or runs into
						// the value: the declaration stays a local, for Luau to report.
						if (typeEnd >= m_Toks.size() || std::any_of(m_Toks.begin() + (std::ptrdiff_t)j, m_Toks.begin() + (std::ptrdiff_t)typeEnd,
															 [](const Token& t) { return t.Is("=") || t.Is(";"); }))
						{
							m_Repl[i] = "local";
							return;
						}
						types.push_back({ j, typeEnd });
						j = Sig(typeEnd);
					}
					if (!m_Toks[j].Is(","))
						break;
					j = Sig(j + 1);
				}
				m_Repl[i] = "";
				for (const auto& [from, to] : types)
					for (size_t k = from; k < to; k++)
						if (!m_Toks[k].IsComment())
							Blank(k);
				if (last != npos && !m_Toks[j].Is("="))
					m_Repl[last] = std::string(m_Toks[last].Text) + " = nil";
			}

			// Leaves token k out of the Luau (its line breaks stay, so every line
			// stays on its line).
			void Blank(size_t k)
			{
				m_Repl[k] = std::string((size_t)(m_Toks[k].EndLine - m_Toks[k].Line), '\n');
				m_Skip[k] = 1;
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
				size_t ValueBegin = npos; // the default's tokens [ValueBegin, ValueEnd)
				size_t ValueEnd = npos;
				ClassField Field;
			};

			// A field default's Luau, for the class profile: the expression
			// translated on its own, as the main pass translates it in place
			// (its line breaks and comments kept).
			// Tokens [begin, end) translated as an expression. With `oneLine`,
			// on one line, without comments.
			std::string TranslateValue(size_t begin, size_t end, bool oneLine = false) const
			{
				return TranslateText(oneLine ? CodeText(begin, end) : SourceText(begin, end));
			}

			// Tokens [begin, end) translated for a typeof, which only reads the
			// type: the parts that are evaluated once or only when needed are
			// written so that each appears once (`a ?? b` as `if false then
			// assert(a) else b`), not again inside a typeof of their own, which
			// would double the text at each level of nesting.
			std::string TypeValue(size_t begin, size_t end) const { return TranslateText(CodeText(begin, end), true); }

			// cluau text translated as an expression.
			std::string TranslateText(const std::string& text, bool typeOnly = false) const
			{
				const std::string key = std::string(m_Options.Script ? "s" : "-") + (m_Options.Manifest ? "m" : "-") + (typeOnly ? "t" : "-") + text;
				if (auto known = t_Values.Entries.find(key); known != t_Values.Entries.end())
				{
					m_Helpers |= known->second.second;
					return known->second.first;
				}
				const uint8_t helpers = m_Helpers;
				m_Helpers = 0;
				std::string value = TranslateTextOnce(text, typeOnly);
				t_Values.Entries[key] = { value, m_Helpers };
				m_Helpers |= helpers;
				return value;
			}

			std::string TranslateTextOnce(const std::string& text, bool typeOnly) const
			{
				TranslateOptions options;
				options.Script = m_Options.Script;
				options.Manifest = m_Options.Manifest;
				const std::string prefix = "return ";
				const TranslateResult r = Translator(prefix + text, options, typeOnly).Run();
				std::string_view luau = r.Luau;
				if (luau.substr(0, kRaiseHead.size()) == kRaiseHead)
					luau.remove_prefix(kRaiseHead.size());
				// The helpers it calls are this chunk's to define.
				for (size_t h = 0; h < std::size(kHelperText); h++)
					if (luau.substr(0, kHelperText[h].size()) == kHelperText[h])
					{
						luau.remove_prefix(kHelperText[h].size());
						m_Helpers |= (uint8_t)(1u << h);
					}
				if (luau.substr(0, prefix.size()) == prefix)
					luau.remove_prefix(prefix.size());
				return Trim(luau);
			}

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
							Blank(j);
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
						!m.ReturnType.empty() ? m.ReturnType : m.ReturnsValue ? std::string("...any") : std::string("()"), m.Generics });
					if (m.Name == "Init")
					{
						members.CtorParams = members.Methods.back().ParamTypes;
						members.CtorGenerics = m.Generics;
					}
				}
				if (!members.CtorParams)
				{
					if (base.empty())
						members.CtorParams = std::string();
					else if (auto inherited = m_CtorParams.find(base); inherited != m_CtorParams.end())
					{
						members.CtorParams = inherited->second.first;
						members.CtorGenerics = inherited->second.second;
					}
				}
				m_CtorParams[m_ClassName] = { members.CtorParams, members.CtorGenerics };
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
						type += m.Name + ": " + m.Generics + "(self: " + m_ClassName + m.ParamTypes + ") -> " + m.ReturnType + ", ";
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
						j = ScanMethod(j, end, info, false);
						continue;
					}
					if (m_Toks[j].Is("async") && m_Toks[Sig(j + 1)].Is("function"))
					{
						j = ScanMethod(Sig(j + 1), end, info, true);
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
						span.Field.Type = CodeText(typeBegin, typeEnd);
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
							if (OpensGroup(vt))
								depth++;
							else if (ClosesGroup(vt))
								depth--;
							v++;
						}
						span.ValueBegin = valueBegin;
						span.ValueEnd = v;
						span.Field.Default = TranslateValue(valueBegin, v);
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
					if (OpensGroup(vt))
						depth++;
					else if (ClosesGroup(vt))
						depth--;
					v++;
				}
				// The runtime keeps a script class's own data under these.
				const std::string_view name = m_Toks[nameTok].Text;
				if (m_Profile == &ScriptProfile())
					for (const char* internal : { "Fields", "__index", "__name", "__base", "__abstract", "__abstracts" })
						if (name == internal)
							Error(m_Toks[nameTok], "`" + std::string(name) + "` is a name the runtime uses in a class: give the static member another name");
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
				signal.Params = CodeText(open + 1, close);
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
				{
					const size_t close = MatchClose(p);
					m.Generics = CodeText(p, close + 1);
					p = Sig(close + 1);
				}
				if (!m_Toks[p].Is("("))
					return Sig(nameTok + 1);
				const size_t close = MatchClose(p);
				m.Params = ParamsText(p, close);
				size_t last = close;
				size_t k = Sig(close + 1);
				if (m_Toks[k].Is(":"))
				{
					const size_t typeEnd = SkipType(k + 1);
					m.ReturnType = CodeText(Sig(k + 1), typeEnd);
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

			size_t ScanMethod(size_t fn, size_t end, ClassInfo& info, bool async)
			{
				ClassMethod m;
				const bool generator = m_Toks[Sig(fn + 1)].Is("*");
				const size_t nameTok = generator ? Sig(Sig(fn + 1) + 1) : Sig(fn + 1);
				if (m_Toks[nameTok].Kind != TokenKind::Name)
					return Sig(fn + 1);
				m.Name = std::string(m_Toks[nameTok].Text);
				m.At = { { m_Toks[nameTok].Line, m_Toks[nameTok].Col }, { m_Toks[nameTok].EndLine, m_Toks[nameTok].EndCol } };
				size_t p = Sig(nameTok + 1);
				if (m_Toks[p].Is("<"))
				{
					const size_t close = MatchClose(p);
					m.Generics = CodeText(p, close + 1);
					p = Sig(close + 1);
				}
				if (!m_Toks[p].Is("("))
					return Sig(nameTok + 1);
				const size_t close = MatchClose(p);
				m.Params = ParamsText(p, close);
				size_t k = Sig(close + 1);
				if (m_Toks[k].Is(":"))
				{
					const size_t typeEnd = SkipType(k + 1);
					m.ReturnType = generator ? GeneratorType(Sig(k + 1), typeEnd)
						: async			 ? TaskType(Sig(k + 1), typeEnd)
										 : CodeText(Sig(k + 1), typeEnd);
					k = Sig(typeEnd);
				}
				if (!m_Toks[k].Is("{"))
					return Sig(k);
				const size_t bodyEnd = MatchClose(k);
				m.BodyBeginLine = m_Toks[k].Line;
				m.BodyEndLine = m_Toks[bodyEnd].Line;
				m.ReturnsValue = generator || async || BodyReturnsValue(k, bodyEnd);
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
							Blank(k);
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
					m_ValueEnd = span.End; // the expression's tokens are ordinary code
					m_PrevSig = i;
					return;
				}
				for (const FieldSpan& span : m_FieldSpans)
				{
					if (span.Begin != i)
						continue;
					auto lower = [&](const std::string& value, std::string& error) {
						ClassField field = span.Field;
						field.Default = value;
						if (m_Profile->LowerField)
							return m_Profile->LowerField(field, m_ClassName, error);
						return value.empty() ? std::string() : m_ClassName + ".Fields." + field.Name + " = " + value;
					};
					std::string error;
					const std::string lowered = lower(span.Field.Default, error);
					if (!error.empty())
						Error(m_Toks[i], error);
					// A lowering that holds the default as it is (where a stand-in for
					// it lands, the rest unchanged): the default's own tokens stay in
					// place and the main pass translates them (so its lines stay on
					// their lines), with the lowering's text before and after them.
					size_t at = std::string::npos;
					std::string after;
					if (!span.Field.Default.empty() && span.ValueBegin != npos)
					{
						static constexpr std::string_view kStandIn = "\x01" "default" "\x01";
						std::string ignored;
						const std::string shape = lower(std::string(kStandIn), ignored);
						at = shape.find(kStandIn);
						if (at != std::string::npos)
						{
							after = shape.substr(at + kStandIn.size());
							if (shape.substr(0, at) + span.Field.Default + after != lowered)
								at = std::string::npos;
						}
					}
					if (at != std::string::npos)
					{
						m_Repl[i] = lowered.substr(0, at);
						for (size_t k = i + 1; k <= span.ValueBegin; k++)
						{
							m_DropGap[k] = 1;
							if (!m_Toks[k].IsComment() && k < span.ValueBegin)
								Blank(k);
						}
						AddAfter(PrevSig(span.ValueEnd), after);
						m_ValueEnd = span.ValueEnd;
						m_PrevSig = PrevSig(span.ValueBegin);
						return;
					}
					if (lowered.find('\n') != std::string::npos)
						Error(m_Toks[i], "this host's class profile lowers " + span.Field.Name + " on one line: write its default on one line");
					m_Repl[i] = lowered;
					for (size_t k = i + 1; k < span.End; k++)
						if (!m_Toks[k].IsComment())
							Blank(k);
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
				if (op == "::")
				{
					MarkTypeEnd(i + 1);
					return;
				}
				if (op == "??=")
				{
					m_NilAssigns.push_back(i); // after the ?? in its value
					return;
				}
				if (op == "...")
				{
					Spread(i);
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

			// `...items` in a call's arguments or a table literal: the items
			// in their place. A call's is its last argument, `f(a,
			// table.unpack(items))`; a table's are gathered as the table
			// closes (TableSpread).
			void Spread(size_t i)
			{
				const Token& t = m_Toks[i];
				if (!StartsOperand(i + 1, t.EndLine) || m_Stack.empty() || m_PrevSig == npos)
					return;
				const Token& prev = m_Toks[m_PrevSig];
				if (!(prev.Is("(") || prev.Is(",") || prev.Is("{") || prev.Is(";")))
					return;
				const Open& frame = m_Stack.back();
				if (frame.Kind == Frame::Table)
				{
					m_TableSpreads[frame.Token].push_back(i);
					return;
				}
				if (frame.Kind != Frame::Paren)
					return;
				const size_t open = frame.Token;
				const size_t before = PrevSig(open);
				// (Other parentheses hold a type's `...T`: `(...number) -> ()`.)
				if (before == open || !EndsExpression(m_Toks[before]) || m_Toks[before].EndLine != m_Toks[open].Line)
					return;
				const size_t close = MatchClose(open);
				for (size_t k = Sig(i + 1); k < close; k = OpensGroup(m_Toks[k]) ? Sig(MatchGroup(k) + 1) : Sig(k + 1))
					if (m_Toks[k].Is(","))
					{
						Error(t, "a spread argument goes last: f(a, ...rest)");
						return;
					}
				Generate(i, "table.unpack(");
				AddAfter(PrevSig(close), ")");
			}

			// A table literal with spreads, `{ first, ...items, size = 2 }`:
			// each spread's list items are added in order where it is, and its
			// other fields copied, later ones replacing earlier ones. The
			// table is __cluau_spread's of its parts (the literal's own runs
			// of items, and the spread values), typed as all of them at once.
			void TableSpread(size_t open, size_t close, const std::vector<size_t>& spreads)
			{
				std::string literal;
				std::string type;
				bool run = true; // a run of the literal's own items is open
				for (size_t k = Sig(open + 1); k < close;)
				{
					size_t end = k;
					while (end < close && !m_Toks[end].Is(",") && !m_Toks[end].Is(";"))
						end = OpensGroup(m_Toks[end]) ? Sig(MatchGroup(end) + 1) : Sig(end + 1);
					const bool spread = std::find(spreads.begin(), spreads.end(), k) != spreads.end();
					if (spread)
					{
						type += (type.empty() ? "" : " & ") + std::string("typeof(") + TypeValue(Sig(k + 1), end) + ")";
						Generate(k, "}, ");
						m_Skip[k] = 1;
						run = false;
						if (end < close)
						{
							Generate(end, ", {");
							run = true;
						}
					}
					else if (end > k)
						literal += (literal.empty() ? "" : ", ") + CodeText(k, end);
					k = end < close ? Sig(end + 1) : close;
				}
				if (!literal.empty())
					type += " & typeof(" + TranslateText("{ " + literal + " }", true) + ")";
				if (m_TypeOnly)
				{
					// Only its type is read: the parts are in the type alone.
					for (size_t k = open + 1; k <= close; k++)
					{
						Blank(k);
						m_Before[k].clear();
						m_After[k].clear();
					}
					Generate(open, "((nil :: any) :: " + type + ")");
					return;
				}
				m_Helpers |= kHelperSpread;
				AddBefore(open, "(__cluau_spread(");
				Generate(close, std::string(run ? "}" : "") + ") :: " + type + ")");
			}

			// `x ??= value`: `if x == nil then x = value end` (the target is a
			// name, or names joined by dots).
			void NilAssign(size_t i)
			{
				if (m_Skip[i])
					return;
				size_t start = i;
				for (size_t k = PrevSig(i); k < i && (m_Toks[k].Kind == TokenKind::Name || m_Toks[k].Is(".")); k = PrevSig(k))
				{
					start = k;
					if (m_StatementStart[k] || k == 0)
						break;
				}
				const size_t last = ExpressionEnd(i);
				if (start == i || !m_StatementStart[start] || !IsPath(start, i) || last == npos)
				{
					Error(m_Toks[i], "??= is a statement: name ??= value");
					return;
				}
				const std::string target = CodeText(start, i);
				AddBefore(start, "if ");
				Generate(i, " == nil then " + target + " =");
				AddAfter(last, " end");
			}

			bool IsStatementStart(size_t i) const
			{
				// Inside brackets or a table constructor nothing starts a
				// statement (`f(g\n{ a = 1 })` is a call with a table).
				if (!m_Stack.empty() && (m_Stack.back().Kind == Frame::Paren || m_Stack.back().Kind == Frame::Bracket ||
					m_Stack.back().Kind == Frame::Table || m_Stack.back().Kind == Frame::Interp))
					return false;
				if (m_PrevSig == npos)
					return true;
				const Token& p = m_Toks[m_PrevSig];
				if (p.Is(";") || m_OpenedBlock[m_PrevSig] || m_ClosedBlock[m_PrevSig])
					return true;
				if (m_ExprIfClose[m_PrevSig])
					return false;
				if (p.Line == m_Toks[i].Line)
					return false;
				return EndsStatement(m_PrevSig);
			}

			// A statement can end at token k: after a value (a name, a literal,
			// a closing bracket), after `x++` / `x--` or `...`, and after a type
			// (`let best: string?`, `type T = Array<number>`), whose last token
			// would otherwise read as an operator still waiting for its operand.
			bool EndsStatement(size_t k) const
			{
				const Token& t = m_Toks[k];
				return EndsExpression(t) || m_TypeEnd[k] || t.Is("++") || t.Is("--") || t.Is("...") || t.Is("?");
			}

			// Marks the end of the type that starts at `begin` (see EndsStatement).
			void MarkTypeEnd(size_t begin)
			{
				const size_t first = Sig(begin);
				const size_t end = SkipType(first);
				if (end > first)
					m_TypeEnd[PrevSig(end)] = 1;
			}

			// `let a: T, b: U`: each annotation's end.
			void MarkDeclarationTypes(size_t keyword)
			{
				size_t j = Sig(keyword + 1);
				while (m_Toks[j].Kind == TokenKind::Name && !m_Toks[j].Is("function"))
				{
					j = Sig(j + 1);
					if (m_Toks[j].Is(":"))
					{
						MarkTypeEnd(j + 1);
						j = Sig(SkipType(j + 1));
					}
					if (!m_Toks[j].Is(","))
						break;
					j = Sig(j + 1);
				}
			}

			// `type Name<T> = ...`: the alias's type.
			void TypeAlias(size_t i)
			{
				size_t k = Sig(Sig(i + 1) + 1);
				if (m_Toks[k].Is("<"))
					k = Sig(MatchClose(k) + 1);
				if (m_Toks[k].Is("="))
					MarkTypeEnd(k + 1);
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
				{
					if (auto spreads = m_TableSpreads.find(open.Token); spreads != m_TableSpreads.end())
						TableSpread(open.Token, i, spreads->second);
					return;
				}
				m_ClosedBlock[i] = 1;
				if (open.Kind == Frame::Class)
				{
					if (m_Options.Script)
					{
						// A local the code after it uses; CloseAtEnd returns it when
						// it is the file's last statement. A `;` after it that would
						// follow the last member's own goes (Luau has no empty statement).
						m_Repl[i] = "";
						m_LastClassEnd = i;
						if (const size_t semi = Sig(i + 1); m_Toks[semi].Is(";") && m_Toks[PrevSig(i)].Is(";"))
							Blank(semi);
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
					const TryState& state = m_Tries[index];
					std::string text = open.Block == BlockKind::Try ? std::string(state.TryVarargs ? "end, ...)" : "end)")
						: open.Block == BlockKind::Catch ? "end, " + TryName(state, "sig") + (state.CatchVarargs ? ", ...) end" : ") end")
						: std::string("end");
					if (open.Block == BlockKind::Try && !more)
						Error(m_Toks[m_Tries[index].Token], "try needs a catch or a finally: try { } catch (error) { }");
					// The statement ends its function: the next token closes the function's block.
					size_t after = Sig(i + 1);
					while (m_Toks[after].Is(";"))
						after = Sig(after + 1);
					const bool lastInFunction = m_Toks[after].Is("}") && !m_Stack.empty() && m_Stack.back().Kind == Frame::Block &&
						(m_Stack.back().Block == BlockKind::Function || m_Stack.back().Block == BlockKind::Method);
					m_TryToken[i] = 1;
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
				case BlockKind::Switch:
					m_Repl[i] = m_Switches[open.Token].Clauses > 0 ? "end end" : "end";
					return;
				default:
					if (auto async = m_AsyncBrace.find(open.Token); async != m_AsyncBrace.end())
						m_Repl[i] = async->second;
					else
						m_Repl[i] = m_GeneratorBrace.count(open.Token) ? "end) end" : "end";
					return;
				}
			}

			// The `}` that ends an interpolated string's segment (an InterpMid
			// opens the next one): what the segment left open is reported.
			void CloseSegment(size_t i)
			{
				if (std::none_of(m_Stack.begin(), m_Stack.end(), [](const Open& o) { return o.Kind == Frame::Interp; }))
					return;
				while (m_Stack.back().Kind != Frame::Interp)
				{
					Error(m_Toks[m_Stack.back().Token], "unclosed '" + std::string(m_Toks[m_Stack.back().Token].Text) + "'");
					m_Stack.pop_back();
				}
				m_Stack.pop_back();
				EndExpressionIfsAbove(m_Stack.size());
				if (m_Toks[i].Kind == TokenKind::InterpMid)
					m_Stack.push_back({ Frame::Interp, BlockKind::Plain, i });
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
				// An if-expression still waiting for its else goes on in more input too.
				m_R.Unfinished = !m_ExprIfs.empty() || !m_Stack.empty() || std::any_of(m_Toks.begin(), m_Toks.end(), [](const Token& t) { return t.Open; });
				while (!m_ExprIfs.empty())
					MissingElse();
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
					case Frame::Interp: break; // the lexer reported the string
					case Frame::Block:
						m_Tail += open.Block == BlockKind::Repeat ? " until false" : " end";
						Error(at, "this '{' has no closing '}'");
						break;
					}
				}
			}

			// The lines holding nothing but try statements' own syntax (and
			// comments): what runs there is the lowering's (the test for an
			// error, the catch block's call, the rethrow), not a statement of
			// the source's. And the lines each try and catch block's function
			// starts on.
			void FindTryLines()
			{
				std::map<int, bool> lines; // line -> only the try's tokens so far
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Token& t = m_Toks[i];
					if (t.Kind == TokenKind::Eof)
						break;
					if (t.IsComment())
						continue;
					for (int line = t.Line; line <= t.EndLine; line++)
					{
						auto [it, added] = lines.emplace(line, m_TryToken[i] != 0);
						if (!added)
							it->second = it->second && m_TryToken[i];
					}
				}
				for (const auto& [line, only] : lines)
					if (only)
						m_R.TryLines.push_back(line);
				std::set<int> starts;
				for (const TryState& t : m_Tries)
				{
					starts.insert(m_Toks[t.Token].Line);
					if (t.CatchAt != npos)
						starts.insert(m_Toks[t.CatchAt].Line);
				}
				m_R.TryFunctionLines.assign(starts.begin(), starts.end());
			}

			// ---- optional chains and ?? -----------------------------------

			// The group closer matching the opener at j (a bracket, or an
			// interpolated string's start).
			size_t MatchGroup(size_t j) const
			{
				if (m_Toks[j].Kind != TokenKind::InterpBegin)
					return MatchClose(j);
				int depth = 0;
				for (size_t k = j; k < m_Toks.size(); k++)
				{
					if (m_Toks[k].Kind == TokenKind::InterpBegin)
						depth++;
					else if (m_Toks[k].Kind == TokenKind::InterpEnd && --depth == 0)
						return k;
				}
				return m_Toks.size() - 1;
			}

			// The group opener matching the closer at j; npos when there is none.
			size_t MatchOpen(size_t j) const
			{
				const bool interp = m_Toks[j].Kind == TokenKind::InterpEnd;
				int depth = 0;
				for (size_t k = j + 1; k-- > 0;)
				{
					const Token& t = m_Toks[k];
					if (interp)
					{
						if (t.Kind == TokenKind::InterpEnd)
							depth++;
						else if (t.Kind == TokenKind::InterpBegin && --depth == 0)
							return k;
						continue;
					}
					if (t.Kind != TokenKind::Op)
						continue;
					if (t.Text == ")" || t.Text == "]" || t.Text == "}")
						depth++;
					else if ((t.Text == "(" || t.Text == "[" || t.Text == "{") && --depth == 0)
						return k;
				}
				return npos;
			}

			// `?` before `.name`, `[` or `:name` with nothing between: an
			// optional chain's step.
			bool IsChainStep(size_t q) const
			{
				if (!m_Toks[q].Is("?") || q + 1 >= m_Toks.size())
					return false;
				const Token& next = m_Toks[q + 1];
				if (next.Begin != m_Toks[q].End)
					return false;
				if (next.Is("["))
					return true;
				return (next.Is(".") || next.Is(":")) && m_Toks[Sig(q + 2)].Kind == TokenKind::Name;
			}

			// Tokens [from, to) are a name or names joined by dots: what can be
			// read twice (`a`, `self.items`).
			bool IsPath(size_t from, size_t to) const
			{
				bool name = true;
				for (size_t k = Sig(from); k < to; k = Sig(k + 1))
				{
					const Token& t = m_Toks[k];
					if (name ? (t.Kind != TokenKind::Name || IsExpressionWord(t.Text) || t.Is("nil") || t.Is("true") || t.Is("false"))
							 : !t.Is("."))
						return false;
					name = !name;
				}
				return !name;
			}

			bool HasVarargs(size_t from, size_t to) const
			{
				for (size_t k = from; k <= to && k < m_Toks.size(); k++)
					if (m_Toks[k].Is("...") && !StartsOperand(k + 1, m_Toks[k].EndLine))
						return true;
				return false;
			}

			// The token after j (comments skipped) begins a value on `line`:
			// what a spread's `...` or an await takes (`...items`, `await f()`).
			bool StartsOperand(size_t j, int line) const
			{
				const Token& t = m_Toks[Sig(j)];
				if (t.Line != line)
					return false;
				if (t.Kind == TokenKind::Name)
					return !IsExpressionWord(t.Text) || t.Is("function");
				return t.Is("(") || t.Is("{") || t.Kind == TokenKind::String || t.Kind == TokenKind::LongString ||
					t.Kind == TokenKind::Interpolated || t.Kind == TokenKind::InterpBegin;
			}

			// The first token of the value an optional chain's `?` at q steps
			// from: a name, a parenthesised expression, or either followed by
			// fields, indexes and calls. npos when there is none.
			size_t ChainReceiver(size_t q) const
			{
				size_t k = PrevSig(q);
				if (k == q)
					return npos;
				for (;;)
				{
					const Token& t = m_Toks[k];
					size_t start = npos;
					if (t.Is(")") || t.Is("]") || t.Kind == TokenKind::InterpEnd)
					{
						const size_t open = MatchOpen(k);
						if (open == npos)
							return npos;
						if (t.Is(")") || t.Is("]"))
						{
							// A call's arguments or an index follow what they apply to,
							// on its line; otherwise the parentheses are the value.
							const size_t before = PrevSig(open);
							if (before != open && m_Toks[open].Is("[") && IsChainStep(before) && PrevSig(before) != before)
							{
								k = PrevSig(before); // `a?[k]`
								continue;
							}
							if (before != open && EndsExpression(m_Toks[before]) && m_Toks[before].EndLine == m_Toks[open].Line &&
								!(m_Toks[open].Is("(") && m_ExprIfClose[before]))
							{
								k = before;
								continue;
							}
							if (t.Is("]"))
								return npos;
						}
						start = open;
					}
					else if (t.Kind == TokenKind::Name && !IsExpressionWord(t.Text))
						start = k;
					else if (t.Kind == TokenKind::String || t.Kind == TokenKind::Interpolated || t.Kind == TokenKind::LongString)
					{
						// A string as a call's argument: `f "x"?.y`.
						const size_t before = PrevSig(k);
						if (before != k && m_Toks[before].Kind == TokenKind::Name && !IsExpressionWord(m_Toks[before].Text))
						{
							k = before;
							continue;
						}
						start = k;
					}
					else
						return npos;
					// A field or method before it: `a.b`, `a:b`.
					const size_t before = PrevSig(start);
					if (before != start && (m_Toks[before].Is(".") || m_Toks[before].Is(":")) && m_Toks[start].Kind == TokenKind::Name)
					{
						size_t owner = PrevSig(before);
						if (owner == before)
							return npos;
						if (IsChainStep(owner)) // `a?.b`
							owner = PrevSig(owner);
						k = owner;
						continue;
					}
					// A chain before it: `a?.b?.c` steps from all of `a?.b`.
					if (before != start && m_Toks[before].Is("?") && IsChainStep(before))
					{
						const size_t owner = PrevSig(before);
						if (owner == before)
							return npos;
						k = owner;
						continue;
					}
					return start;
				}
			}

			// The last token of the optional chain whose step is the `?` at q:
			// its fields, indexes and calls up to the next step, if any.
			size_t ChainEnd(size_t q) const
			{
				size_t last = q;
				size_t k = q + 1;
				for (;;)
				{
					const Token& t = m_Toks[k];
					if (t.Is(".") || t.Is(":"))
					{
						const size_t name = Sig(k + 1);
						if (m_Toks[name].Kind != TokenKind::Name)
							return last;
						last = name;
						k = Sig(name + 1);
						continue;
					}
					if (t.Is("["))
					{
						last = MatchClose(k);
						k = Sig(last + 1);
						continue;
					}
					if (t.Is("(") && last != q && t.Line == m_Toks[last].EndLine)
					{
						last = MatchClose(k);
						k = Sig(last + 1);
						continue;
					}
					return last;
				}
			}

			// `a?.b`, `a?[k]`, `a?:m()`: nil when `a` is, without reading
			// further; otherwise `a.b`, `a[k]`, `a:m()`. A name (or names
			// joined by dots) is tested where it is, `(if a == nil then nil
			// else a.b)`, and a statement of one is an if statement; anything
			// else is evaluated once, by __cluau_chain.
			void Chains()
			{
				for (size_t q = 0; q < m_Toks.size(); q++)
				{
					if (!IsChainStep(q) || m_Skip[q])
						continue;
					const size_t start = ChainReceiver(q);
					if (start == npos)
					{
						Error(m_Toks[q], "?. follows a value: a?.b, a?[k], a?:m()");
						continue;
					}
					const size_t last = ChainEnd(q);
					if (m_Toks[q + 1].Is(":") && !m_Toks[Sig(last == q ? q + 1 : last)].Is(")"))
					{
						Error(m_Toks[q], "?: calls a method: a?:m()");
						continue;
					}
					const Token& after = m_Toks[Sig(last + 1)];
					static constexpr const char* kAssign[] = { "=", "+=", "-=", "*=", "/=", "%=", "^=", "..=", "~/=", "++", "--" };
					if (std::any_of(std::begin(kAssign), std::end(kAssign), [&](const char* op) { return after.Is(op); }))
					{
						Error(m_Toks[q], "an optional chain is a value: it cannot be assigned to");
						continue;
					}
					if (IsPath(start, q))
					{
						const std::string receiver = CodeText(start, q);
						const bool statement = m_StatementStart[Sig(start)] &&
							(after.Kind == TokenKind::Eof || after.Is(";") || after.Is("}") || after.Line > m_Toks[last].EndLine);
						if (statement)
						{
							AddBefore(Sig(start), "if ");
							Generate(q, " ~= nil then " + receiver);
							AddAfter(last, " end");
						}
						else
						{
							AddBefore(Sig(start), "(if ");
							Generate(q, " == nil then nil else " + receiver);
							AddAfter(last, ")");
						}
						continue;
					}
					if (HasVarargs(start, last))
					{
						Error(m_Toks[q], "this optional chain cannot use ... (its steps are read in a function of their own)");
						continue;
					}
					if (m_TypeOnly)
					{
						// For a typeof: the steps read from the value without nil.
						AddBefore(Sig(start), "((nil :: any) :: typeof(assert(");
						Generate(q, ")");
						AddAfter(last, ")?)");
						continue;
					}
					// The function's parameter has the value's type without nil
					// (assert's), so what the chain reads from it is checked.
					m_Helpers |= kHelperChain;
					AddBefore(Sig(start), "__cluau_chain(");
					Generate(q, ", function(__cluau_v: typeof(assert(" + TypeValue(start, q) + "))) return __cluau_v");
					AddAfter(last, " end)");
				}
			}

			// A `??`'s left side: back to where its expression starts (`??`
			// binds more loosely than any other operator).
			size_t CoalesceStart(size_t op) const
			{
				size_t start = npos;
				size_t k = op;
				while (k > 0)
				{
					const size_t p = PrevSig(k);
					if (p == k)
						break;
					const Token& t = m_Toks[p];
					if (t.Is(")") || t.Is("]") || t.Is("}") || t.Kind == TokenKind::InterpEnd)
					{
						if (m_ExprIfClose[p])
							break; // an if-expression's `then`
						const size_t open = MatchOpen(p);
						if (open == npos)
							break;
						start = open;
						k = open;
						continue;
					}
					if (OpensGroup(t) || t.Kind == TokenKind::InterpMid || t.Is(",") || t.Is(";") || t.Is("??") || t.Is("="))
						break;
					if (t.Kind == TokenKind::Op && t.Text.size() >= 2 && t.Text.back() == '=' && t.Text != "==" && t.Text != "~=" && t.Text != "!=" &&
						t.Text != "<=" && t.Text != ">=")
						break; // a compound assignment
					if (t.Kind == TokenKind::Name && (IsExpressionWord(t.Text) && !t.Is("and") && !t.Is("or") && !t.Is("not") && !t.Is("function")))
						break;
					if (t.Kind == TokenKind::Name && (t.Is("yield") || t.Is("throw") || t.Is("case")) && m_Repl[p])
						break;
					if (t.Is("...") && m_Repl[p])
						break; // a spread

					if (m_StatementStart[p] && p != Sig(0) && start != npos && m_Toks[p].Line < m_Toks[start].Line)
						break;
					start = p;
					k = p;
				}
				return start;
			}

			// A `??`'s right side: its last token (to the end of the
			// expression, another `??` included).
			size_t CoalesceEnd(size_t op) const
			{
				size_t last = npos;
				int ifs = 0;
				for (size_t k = Sig(op + 1);;)
				{
					const Token& t = m_Toks[k];
					if (t.Kind == TokenKind::Eof || t.Is(";") || t.Is(",") || t.Is(")") || t.Is("]") || t.Is("}") || t.Is("=") ||
						t.Kind == TokenKind::InterpMid || t.Kind == TokenKind::InterpEnd || m_CaseColon.count(k))
						break;
					if (last != npos && t.Line > m_Toks[last].EndLine && !AwaitsOperand(m_Toks[last]) && !ContinuesLine(t) && !m_ExprIfClose[last] &&
						!m_Toks[last].Is("else") && !(t.Is("else") && ifs > 0))
						break;
					if (t.Is("else"))
					{
						if (ifs == 0)
							break;
						ifs--;
					}
					else if (t.Is("if") && m_Toks[Sig(k + 1)].Is("("))
						ifs++;
					if (t.Is("::"))
					{
						const size_t typeEnd = SkipType(k + 1);
						if (typeEnd > Sig(k + 1))
						{
							last = PrevSig(typeEnd);
							k = Sig(typeEnd);
							continue;
						}
					}
					if (OpensGroup(t))
					{
						last = MatchGroup(k);
						k = Sig(last + 1);
						continue;
					}
					last = k;
					k = Sig(k + 1);
				}
				return last;
			}

			// `a ?? b`: `a` unless it is nil, then `b` (evaluated only then). A
			// name (or names joined by dots) is tested where it is, `(if a ~=
			// nil then a else b)`; anything else is evaluated once, by
			// __cluau_nil. From the last: an inner one's closing text comes
			// first.
			void Coalesces()
			{
				for (size_t op = m_Toks.size(); op-- > 0;)
				{
					if (!m_Toks[op].Is("??") || m_Skip[op])
						continue;
					const size_t start = CoalesceStart(op);
					const size_t last = CoalesceEnd(op);
					if (start == npos || last == npos)
					{
						Error(m_Toks[op], "?? goes between two values: a ?? b");
						continue;
					}
					if (IsPath(start, op))
					{
						AddBefore(Sig(start), "(if ");
						Generate(op, " ~= nil then " + CodeText(start, op) + " else ");
						AddAfter(last, ")");
						continue;
					}
					if (m_TypeOnly)
					{
						// For a typeof: either side's type, each written once.
						AddBefore(Sig(start), "(if false then assert(");
						Generate(op, ") else ");
						AddAfter(last, ")");
						continue;
					}
					// A literal or a name is read at once, costing nothing; anything
					// else is evaluated only when needed, in a function. The result
					// is typed as the left side without nil or the right side
					// (typeof does not evaluate them again).
					const size_t right = Sig(op + 1);
					const std::string type = ") :: typeof(assert(" + TypeValue(start, op) + ")) | typeof(" + TypeValue(right, last + 1) + "))";
					const Token& r = m_Toks[right];
					const bool literal = right == last && (r.Kind == TokenKind::Number || r.Kind == TokenKind::String ||
															  r.Kind == TokenKind::LongString || r.Is("true") || r.Is("false") || r.Is("nil"));
					if (literal || IsPath(right, last + 1))
					{
						m_Helpers |= kHelperOr;
						AddBefore(Sig(start), "(__cluau_or(");
						Generate(op, ", ");
						AddAfter(last, type);
						continue;
					}
					if (HasVarargs(op, last))
					{
						Error(m_Toks[op], "the right side of ?? cannot use ... (it is evaluated in a function of its own)");
						continue;
					}
					m_Helpers |= kHelperNil;
					AddBefore(Sig(start), "(__cluau_nil(");
					Generate(op, ", function() return ");
					AddAfter(last, " end" + type);
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
				if (m_RaiseUsed)
				{
					// Before anything a name could hide it with, on the first line.
					m_R.Map.AddAnchor(0, 0, 0);
					m_R.Generated.push_back({ { 0, 0 }, { 0, (int)kRaiseHead.size() } });
					append(kRaiseHead);
					m_R.Map.AddAnchor(0, 0, outCol);
				}
				for (size_t h = 0; h < std::size(kHelperText); h++)
					if (m_Helpers & (1u << h))
					{
						m_R.Map.AddAnchor(0, 0, outCol);
						m_R.Generated.push_back({ { 0, outCol }, { 0, outCol + (int)kHelperText[h].size() } });
						append(kHelperText[h]);
						m_R.Map.AddAnchor(0, 0, outCol);
					}
				for (size_t i = 0; i < m_Toks.size(); i++)
				{
					const Token& t = m_Toks[i];
					std::string_view gap = m_Src.substr(pos, t.Begin - pos);
					if (pos == 0 && gap.substr(0, 3) == "\xEF\xBB\xBF")
					{
						// A byte order mark is not Luau.
						gap.remove_prefix(3);
						m_R.Map.AddAnchor(0, 3, outCol);
					}
					if (!m_DropGap[i] || gap.find('\n') != std::string_view::npos)
						append(gap);
					else if (!gap.empty())
						m_R.Map.AddAnchor(t.Line, t.Col, outCol); // the token moved left
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
					// A `-` before a comment's `--` would make `---`, a comment from the
					// minus on.
					if (!text.empty() && text.front() == '-' && !out.empty() && out.back() == '-')
						append(" ");
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
					if (!m_Before[i].empty())
					{
						if (isWord(m_Before[i].front()) && !out.empty() && isWord(out.back()))
							append(" ");
						m_R.Generated.push_back({ { t.Line, outCol }, { t.Line, outCol + (int)m_Before[i].size() } });
						append(m_Before[i]);
						m_R.Map.AddAnchor(t.Line, t.Col, outCol);
					}
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
					// A token spanning lines that is replaced (a long string an
					// error drops) keeps its line breaks, so every line after it
					// stays on its line.
					for (int missing = t.EndLine - t.Line - (int)std::count(text.begin(), text.end(), '\n'); missing > 0; missing--)
						append("\n");
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
			std::vector<std::string> m_Before; // generated text before a token (its replacement)
			std::vector<uint8_t> m_StatementStart; // the token starts a statement
			std::set<size_t> m_CaseColon;          // a switch case's `:`
			std::set<size_t> m_GeneratorBrace;     // a generator's body's `{`
			std::map<size_t, std::string> m_AsyncBrace; // an async function's body's `{` -> what its `}` becomes
			std::map<size_t, std::vector<size_t>> m_TableSpreads; // a table's `{` -> its spreads' `...`
			std::vector<size_t> m_Awaits;          // the `await` tokens
			bool m_AsyncNext = false;              // the next function is async
			std::vector<size_t> m_NilAssigns;      // the `??=` tokens
			struct SwitchState
			{
				std::string Name;    // the local holding the value
				std::string Subject; // what the cases compare (the local, or the value's name)
				int Clauses = 0;
				bool Default = false;
				bool Reported = false;
			};
			std::map<size_t, SwitchState> m_Switches; // a switch's `{` -> its state
			std::vector<BlockMark> m_BlockAt;
			std::vector<uint8_t> m_OpenedBlock;
			std::vector<uint8_t> m_ClosedBlock;
			std::vector<uint8_t> m_ClosedIf;
			std::vector<uint8_t> m_ClosedRepeat;
			std::vector<uint8_t> m_ExprIfClose; // an if-expression condition's `)`
			std::vector<uint8_t> m_TypeEnd;     // the last token of a type that can end a statement (`let x: T?`, `type T = A<B>`)
			std::vector<uint8_t> m_Skip;
			std::vector<uint8_t> m_DropGap; // the spaces before this token are not copied (a line break always is)
			std::vector<uint8_t> m_ThrowAt; // a `throw` statement's keyword: its value follows
			std::vector<uint8_t> m_TryToken; // the try statement's own syntax: `}` of its blocks, `catch (e: T) {`, `finally {`
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
			size_t m_ValueEnd = npos; // the end of a static's or a field default's expression (ordinary code, not class items)
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
			std::map<size_t, std::string> m_VarargType; // a function's `{` -> the type its `...: T` declares
			std::map<size_t, std::pair<size_t, BlockKind>> m_TryClosed; // a try's or catch's `}` that a catch / finally follows
			std::string m_ClassName;
			std::string m_ClassBase; // the class being translated's base ("" without one)
			std::map<std::string, std::pair<std::optional<std::string>, std::string>> m_CtorParams; // each class's constructor parameters (and generics), for its subclasses
			size_t m_ClassEndToken = npos;
			bool m_ClassEnded = false;
			size_t m_LastClassEnd = npos; // script mode: the last class's `}`
			size_t m_PrevSig = npos;
			size_t m_MethodHeaderEnd = npos;
			mutable bool m_RaiseUsed = false; // host mode: the chunk takes Luau's `error` as __cluau_error
			mutable uint8_t m_Helpers = 0;    // the Helper functions the chunk calls
			bool m_TypeOnly = false;          // the chunk is an expression a typeof reads (never run)
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
		struct Scope
		{
			Scope() { t_Values.Depth++; }
			~Scope()
			{
				if (--t_Values.Depth == 0)
					t_Values.Entries.clear();
			}
		} scope;
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
					meta = "__call: " + members.CtorGenerics + "(unknown" + ctor + ") -> $Class";
				if (derived)
					meta += (meta.empty() ? "" : ", ") + std::string("__index: typeof(") + info.Base + ")";
				// `for (name, value in Class)`: the class's own statics and
				// methods, by name (the runtime's __iter).
				meta += (meta.empty() ? "" : ", ") + std::string("__iter: (unknown) -> ((unknown, string?) -> (string?, unknown), unknown)");
				h.TableInit = "setmetatable({ Fields = {}, __name = \"$Class\"" + (derived ? ", __base = " + info.Base : std::string()) +
					(info.Abstract ? ", __abstract = true" : "") + (abstracts.empty() ? "" : ", __abstracts = { " + abstracts + " }") +
					", __instance = (nil :: any) :: $Class }, __cluau_meta :: { " + meta + " })";
				// Instances find their methods and statics in the class table
				// (`__index`: its type, which the methods and statics after
				// this line extend), and their metamethods in their metatable.
				std::string metamethods;
				for (const ClassMembers::Method& m : members.Methods)
					if (m.Name.rfind("__", 0) == 0)
						metamethods += m.Name + ": " + m.Generics + "(self: $Class" + m.ParamTypes + ") -> " + m.ReturnType + ", ";
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
		return "0.33.1";
	}

} // namespace cluau
