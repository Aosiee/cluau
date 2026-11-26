#include "cluau/check.h"

#include "Luau/Ast.h"
#include "Luau/Module.h"
#include "Luau/Type.h"
#include "Luau/TypePack.h"

#include <algorithm>
#include <map>
#include <set>

namespace cluau::check {

	namespace {

		bool IsAny(Luau::TypeId type)
		{
			return Luau::get<Luau::AnyType>(Luau::follow(type)) != nullptr;
		}

		// Unannotated and not pinned down by context: an inferred any or a
		// fresh generic.
		bool IsLoose(Luau::TypeId type)
		{
			type = Luau::follow(type);
			return Luau::get<Luau::AnyType>(type) || Luau::get<Luau::GenericType>(type) || Luau::get<Luau::FreeType>(type);
		}

		std::string NameOf(const Luau::AstName& name) { return name.value ? name.value : ""; }

		// A function body's own `return <value>` (nested functions excluded).
		struct ReturnFinder : Luau::AstVisitor
		{
			bool Found = false;
			bool visit(Luau::AstExprFunction*) override { return false; }
			bool visit(Luau::AstStatReturn* ret) override
			{
				if (ret->list.size > 0)
					Found = true;
				return false;
			}
		};

		class Rules : public Luau::AstVisitor
		{
		public:
			Rules(const Luau::Module& module, const TranslateResult& translation)
				: m_Module(module), m_Translation(translation)
			{
				for (const Exemption& e : translation.Exemptions)
					m_Exempt[e.Line] = false;
				// The const locals, by their Luau position.
				for (const Range& r : translation.ConstNames)
				{
					const Position luau = translation.Map.ToLuau(r.Begin);
					m_ConstPositions.insert({ luau.Line, luau.Col });
				}
			}

			std::vector<Finding> Findings;

			void Finish()
			{
				// Any-typed values the script uses, one finding per line: the
				// first on it, and of those starting there the shortest (`m`
				// of `m.f(x)`), whatever order the map holds them in.
				std::vector<Luau::Location> anys;
				for (const auto& [expr, type] : m_Module.astTypes)
					if (expr && IsAny(type))
						anys.push_back(expr->location);
				std::sort(anys.begin(), anys.end(), [](const Luau::Location& a, const Luau::Location& b) {
					return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
				});
				for (const Luau::Location& location : anys)
					NoteAny(location, "a value of type any");
				for (const auto& [line, used] : m_Exempt)
					if (!used)
					{
						Finding f;
						f.At = { { line, 0 }, { line, 0 } };
						f.Code = "UnusedExemption";
						f.Message = "this allow-any exemption covers nothing - remove it";
						f.Warning = true;
						Findings.push_back(std::move(f));
					}
				std::stable_sort(Findings.begin(), Findings.end(), [](const Finding& a, const Finding& b) { return a.At.Begin < b.At.Begin; });
			}

			bool visit(Luau::AstType*) override { return true; }

			bool visit(Luau::AstTypeReference* type) override
			{
				if (!type->prefix && NameOf(type->name) == "any")
					NoteAny(type->location, "an explicit any");
				return true;
			}

			void NoteLocal(Luau::AstLocal* local)
			{
				if (m_ConstPositions.count({ (int)local->location.begin.line, (int)local->location.begin.column }))
					m_Consts.insert(local);
			}

			bool visit(Luau::AstStatLocal* stat) override
			{
				for (Luau::AstLocal* local : stat->vars)
					NoteLocal(local);
				// `const f = function(...) { ... }` is a declared function.
				for (size_t i = 0; i < stat->values.size && i < stat->vars.size; i++)
					if (Luau::AstExprFunction* fn = stat->values.data[i]->as<Luau::AstExprFunction>())
					{
						m_Declared.insert(fn);
						CheckReturn(fn, stat->vars.data[i]->location);
					}
				return true;
			}

			bool visit(Luau::AstStatAssign* stat) override
			{
				for (Luau::AstExpr* target : stat->vars)
					CheckConstTarget(target);
				return true;
			}

			bool visit(Luau::AstStatCompoundAssign* stat) override
			{
				CheckConstTarget(stat->var);
				return true;
			}

			bool visit(Luau::AstExprFunction* fn) override
			{
				const Luau::TypeId* fnType = m_Module.astTypes.find(fn);
				const Luau::FunctionType* ftv = fnType ? Luau::get<Luau::FunctionType>(Luau::follow(*fnType)) : nullptr;
				std::vector<Luau::TypeId> params;
				if (ftv)
					params = Luau::flatten(ftv->argTypes).first;
				const size_t offset = fn->self ? 1 : 0; // a method's implicit self
				for (size_t i = 0; i < fn->args.size; i++)
				{
					Luau::AstLocal* arg = fn->args.data[i];
					if (arg->annotation || NameOf(arg->name) == "self")
						continue;
					const size_t at = i + offset;
					if (at < params.size() && !IsLoose(params[at]))
						continue; // its type came from context
					Note(arg->location, "AnnotateParameter", "annotate the parameter '" + NameOf(arg->name) + "' (strict typing)");
				}
				if (fn->vararg && !fn->varargAnnotation)
					Note(fn->varargLocation, "AnnotateParameter", "annotate the variadic parameter: ...: Type (strict typing)");
				// A function expression whose type no context gives (a
				// callback's comes from the parameter it is passed to): its
				// return type is annotated, as a declared function's is.
				if (!m_Declared.count(fn))
				{
					const Luau::TypeId* expected = m_Module.astExpectedTypes.find(fn);
					if (!expected || IsLoose(*expected))
					{
						const Luau::Position begin = fn->location.begin;
						CheckReturn(fn, Luau::Location(begin, Luau::Position(begin.line, begin.column + 8))); // `function`
					}
				}
				return true;
			}

			bool visit(Luau::AstStatFunction* stat) override
			{
				m_Declared.insert(stat->func);
				CheckReturn(stat->func, stat->name->location);
				return true;
			}

			bool visit(Luau::AstStatLocalFunction* stat) override
			{
				m_Declared.insert(stat->func);
				CheckReturn(stat->func, stat->name->location);
				return true;
			}

		private:
			bool Generated(const Luau::Location& location) const
			{
				for (const Range& g : m_Translation.Generated)
					if ((int)location.begin.line == g.Begin.Line && (int)location.begin.column >= g.Begin.Col && (int)location.begin.column < g.End.Col)
						return true;
				return false;
			}

			Range ToSource(const Luau::Location& location) const
			{
				const Position begin = m_Translation.Map.FromLuau({ (int)location.begin.line, (int)location.begin.column });
				const Position end = m_Translation.Map.FromLuau({ (int)location.end.line, (int)location.end.column });
				return { begin, end };
			}

			void Note(const Luau::Location& location, const char* code, std::string message, bool warning = false)
			{
				if (Generated(location))
					return;
				Finding f;
				f.At = ToSource(location);
				f.Code = code;
				f.Message = std::move(message);
				f.Warning = warning;
				Findings.push_back(std::move(f));
			}

			void NoteAny(const Luau::Location& location, const char* what)
			{
				if (Generated(location))
					return;
				const int line = (int)location.begin.line;
				if (auto exempt = m_Exempt.find(line); exempt != m_Exempt.end())
				{
					exempt->second = true;
					return;
				}
				if (!m_AnyLines.insert(line).second)
					return;
				Note(location, "NoAny", std::string(what) + " - strict typing allows any only with a line exemption: // allow-any: <reason>");
			}

			void CheckConstTarget(Luau::AstExpr* target)
			{
				if (Luau::AstExprLocal* local = target->as<Luau::AstExprLocal>())
					if (m_Consts.count(local->local))
						Note(target->location, "ConstAssign", "'" + NameOf(local->local->name) + "' is const and cannot be assigned");
			}

			void CheckReturn(Luau::AstExprFunction* fn, const Luau::Location& nameLocation)
			{
				if (fn->returnAnnotation)
					return;
				ReturnFinder finder;
				fn->body->visit(&finder);
				if (finder.Found)
					Note(nameLocation, "AnnotateReturn", "this function returns a value: annotate its return type (strict typing)");
			}

			const Luau::Module& m_Module;
			const TranslateResult& m_Translation;
			std::map<int, bool> m_Exempt; // line -> used
			std::set<std::pair<int, int>> m_ConstPositions;
			std::set<Luau::AstLocal*> m_Consts;
			std::set<int> m_AnyLines;
			std::set<const Luau::AstExprFunction*> m_Declared; // the functions of function statements
		};

	} // namespace

	std::vector<Finding> Run(const Luau::SourceModule& source, const Luau::Module& module, const TranslateResult& translation)
	{
		Rules rules(module, translation);
		if (source.root)
			source.root->visit(&rules);
		rules.Finish();
		return std::move(rules.Findings);
	}

} // namespace cluau::check
