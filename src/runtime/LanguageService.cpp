#include "LanguageService.h"

#include "cluau/format.h"
#include "Resolve.h"
#include "Std.h"

#include "Luau/AstQuery.h"
#include "Luau/Autocomplete.h"
#include "Luau/Frontend.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "Luau/Type.h"
#include "Luau/TypePack.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		bool IsIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

		std::string NameOf(const Luau::AstName& name) { return name.value ? std::string(name.value) : std::string(); }

		bool IsCluauPath(const std::string& name) { return !name.empty() && name[0] != '@' && !IsLuauFile(fs::u8path(name)); }

		std::string LineOf(const SourceText& text, int line) { return std::string(text.Line(line)); }

		// The name a global has when the script does not declare it.
		constexpr const char* kRuntimeGlobal = "@globals";

		std::string TypeString(Luau::TypeId type)
		{
			Luau::ToStringOptions options;
			options.functionTypeArguments = true;
			options.maxTableLength = 8;
			options.maxTypeLength = 200;
			return Luau::toString(Luau::follow(type), options);
		}

		std::string TypePackString(Luau::TypePackId pack)
		{
			Luau::ToStringOptions options;
			options.maxTypeLength = 120;
			return Luau::toString(pack, options);
		}

		// Luau's block words, which cluau does not use.
		// The standard modules this runtime has (as `require("@std/...")`
		// finds them): "fs", "json", ...
		const std::vector<std::string>& StdModuleNames()
		{
			static const std::vector<std::string> names = [] {
				std::vector<std::string> out;
				const std::string all = detail::StdNames();
				for (size_t at = 0; (at = all.find("@std/", at)) != std::string::npos;)
				{
					at += 5;
					const size_t end = all.find(',', at);
					out.push_back(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
				}
				return out;
			}();
			return names;
		}

		// The word before `col` on the line ("" for none): `class` in `class Point`.
		std::string WordBefore(const std::string& line, int col)
		{
			int end = std::min(col, (int)line.size());
			while (end > 0 && std::isspace((unsigned char)line[(size_t)end - 1]))
				end--;
			int begin = end;
			while (begin > 0 && (std::isalnum((unsigned char)line[(size_t)begin - 1]) || line[(size_t)begin - 1] == '_'))
				begin--;
			return line.substr((size_t)begin, (size_t)(end - begin));
		}

		// Inside `require("...` on the line, before `col`: where the path's text begins (-1 otherwise).
		int RequirePathAt(const std::string& line, int col)
		{
			col = std::min(col, (int)line.size());
			for (int i = col - 1; i >= 0; i--)
			{
				const char c = line[(size_t)i];
				if (c == '"' || c == '\'')
				{
					int j = i - 1;
					while (j >= 0 && std::isspace((unsigned char)line[(size_t)j]))
						j--;
					if (j < 0 || line[(size_t)j] != '(')
						return -1;
					j--;
					while (j >= 0 && std::isspace((unsigned char)line[(size_t)j]))
						j--;
					const int end = j + 1;
					while (j >= 0 && (std::isalnum((unsigned char)line[(size_t)j]) || line[(size_t)j] == '_'))
						j--;
					return line.compare((size_t)(j + 1), (size_t)(end - j - 1), "require") == 0 && end - j - 1 == 7 ? i + 1 : -1;
				}
				if (c == '(' || c == ')')
					return -1;
			}
			return -1;
		}

		bool IsLuauOnlyKeyword(const std::string& word)
		{
			return word == "then" || word == "end" || word == "elseif" || word == "repeat" || word == "until" || word == "local";
		}

		bool IsKeyword(const std::string& word)
		{
			static const std::set<std::string> kKeywords = { "and", "break", "class", "const", "continue", "do", "else", "enum", "extends",
				"false", "for", "function", "if", "in", "let", "nil", "not", "or", "return", "static", "true", "while", "abstract", "local",
				"then", "end", "elseif", "repeat", "until", "export", "type", "typeof" };
			return kKeywords.count(word) > 0;
		}

		bool IsIdentifier(const std::string& name)
		{
			if (name.empty() || std::isdigit((unsigned char)name[0]))
				return false;
			return std::all_of(name.begin(), name.end(), IsIdent);
		}

		// The name stands at (line, col) in the text.
		bool IdentifierAt(const SourceText& text, int line, int col, const std::string& name)
		{
			const std::string_view lineText = text.Line(line);
			if (col < 0 || (size_t)col + name.size() > lineText.size() || lineText.compare((size_t)col, name.size(), name) != 0)
				return false;
			const bool before = col == 0 || !IsIdent(lineText[(size_t)col - 1]);
			const bool after = (size_t)col + name.size() == lineText.size() || !IsIdent(lineText[(size_t)col + name.size()]);
			return before && after;
		}

		Luau::TypeId StripOptional(Luau::TypeId type)
		{
			type = Luau::follow(type);
			if (const Luau::UnionType* u = Luau::get<Luau::UnionType>(type))
			{
				std::optional<Luau::TypeId> only;
				for (Luau::TypeId part : u->options)
				{
					part = Luau::follow(part);
					if (Luau::isNil(part))
						continue;
					if (only)
						return type;
					only = part;
				}
				if (only)
					return *only;
			}
			return type;
		}

		// The comment lines right above a declaration's line.
		std::string DocAbove(const SourceText& text, int declLine)
		{
			std::vector<std::string> lines;
			for (int l = declLine - 1; l >= 0; l--)
			{
				std::string line = LineOf(text, l);
				const size_t first = line.find_first_not_of(" \t");
				if (first == std::string::npos || (line.compare(first, 2, "//") != 0 && line.compare(first, 2, "--") != 0))
					break;
				line = line.substr(first + 2);
				while (!line.empty() && (line.front() == '/' || line.front() == '-' || line.front() == ' '))
					line.erase(line.begin());
				lines.push_back(line);
			}
			std::string doc;
			for (auto it = lines.rbegin(); it != lines.rend(); ++it)
				doc += (doc.empty() ? "" : "\n") + *it;
			return doc;
		}

		// A table type's property: its type, and the module and place it
		// was declared (an instance's methods are its metatable's __index's).
		struct Member
		{
			const Luau::Property* Prop = nullptr;
			std::string Module;
		};

		Member FindMember(Luau::TypeId type, const std::string& name, int depth = 0)
		{
			if (depth > 8)
				return {};
			type = StripOptional(type);
			if (const Luau::TableType* table = Luau::get<Luau::TableType>(type))
			{
				if (auto prop = table->props.find(name); prop != table->props.end())
					return { &prop->second, table->definitionModuleName };
				return {};
			}
			if (const Luau::MetatableType* meta = Luau::get<Luau::MetatableType>(type))
			{
				if (Member m = FindMember(meta->table, name, depth + 1); m.Prop)
					return m;
				if (const Luau::TableType* mt = Luau::get<Luau::TableType>(Luau::follow(meta->metatable)))
					if (auto index = mt->props.find("__index"); index != mt->props.end() && index->second.readTy)
						return FindMember(*index->second.readTy, name, depth + 1);
				return {};
			}
			if (const Luau::IntersectionType* parts = Luau::get<Luau::IntersectionType>(type))
				for (Luau::TypeId part : parts->parts)
					if (Member m = FindMember(part, name, depth + 1); m.Prop)
						return m;
			return {};
		}

		struct LocalUses : Luau::AstVisitor
		{
			Luau::AstLocal* Target = nullptr;
			std::vector<Luau::Location> Found;
			bool visit(Luau::AstExprLocal* node) override
			{
				if (node->local == Target)
					Found.push_back(node->location);
				return true;
			}
		};

	} // namespace

	SourceText::SourceText(std::string text)
		: Text(std::move(text))
	{
		Starts.push_back(0);
		for (size_t i = 0; i < Text.size(); i++)
			if (Text[i] == '\n')
				Starts.push_back(i + 1);
	}

	std::string_view SourceText::Line(int line) const
	{
		if (line < 0 || (size_t)line >= Starts.size())
			return {};
		const size_t start = Starts[(size_t)line];
		size_t end = (size_t)line + 1 < Starts.size() ? Starts[(size_t)line + 1] - 1 : Text.size();
		if (end > start && Text[end - 1] == '\r')
			end--;
		return std::string_view(Text).substr(start, end - start);
	}

	LanguageService::LanguageService(const CheckOptions& options)
		: m_Analysis(options)
	{
	}

	std::shared_ptr<const SourceText> LanguageService::SourceOf(const std::string& name) const
	{
		if (auto it = m_Sources.find(name); it != m_Sources.end())
			return it->second;
		std::optional<std::string> text = m_Analysis.TextOf(name);
		std::shared_ptr<const SourceText> source = text ? std::make_shared<const SourceText>(std::move(*text)) : nullptr;
		m_Sources[name] = source;
		return source;
	}

	std::shared_ptr<const SourceText> LanguageService::Source(const std::string& path) const
	{
		if (auto it = m_SourcesByPath.find(path); it != m_SourcesByPath.end())
			return it->second;
		return m_SourcesByPath[path] = SourceOf(Name(path));
	}

	std::string LanguageService::Name(const std::string& path) const { return ModuleKey(fs::u8path(path)); }

	void LanguageService::Open(const std::string& path, std::string text)
	{
		ForgetTexts();
		const std::string name = m_Analysis.AddRoot(fs::u8path(path));
		m_Open[name] = path;
		m_Analysis.SetBuffer(name, std::move(text));
	}

	void LanguageService::Change(const std::string& path, std::string text)
	{
		ForgetTexts();
		const std::string name = Name(path);
		if (!m_Open.count(name))
			return Open(path, std::move(text));
		m_Analysis.SetBuffer(name, std::move(text));
	}

	void LanguageService::Close(const std::string& path)
	{
		ForgetTexts();
		const std::string name = Name(path);
		m_Open.erase(name);
		m_Analysis.SetBuffer(name, std::nullopt);
	}

	void LanguageService::Changed(const std::string& path)
	{
		ForgetTexts();
		m_Analysis.MarkDirty(Name(path));
	}

	bool LanguageService::IsOpen(const std::string& path) const { return m_Open.count(Name(path)) > 0; }

	std::vector<std::string> LanguageService::OpenFiles() const
	{
		std::vector<std::string> out;
		for (const auto& [name, path] : m_Open)
			out.push_back(path);
		return out;
	}

	std::optional<std::string> LanguageService::TextOf(const std::string& path) const { return m_Analysis.TextOf(Name(path)); }

	std::map<std::string, std::vector<CheckFinding>> LanguageService::Diagnostics()
	{
		std::map<std::string, std::vector<CheckFinding>> out;
		for (const auto& [name, path] : m_Open)
		{
			m_Analysis.Frontend().check(name);
			out[path] = m_Analysis.Findings(name);
		}
		return out;
	}

	Luau::Position LanguageService::In(const std::string& name, int line, int col) const
	{
		Position at{ std::max(0, line), std::max(0, col) };
		if (const TranslateResult* t = m_Analysis.Translation(name))
			at = t->Map.ToLuau(at);
		return Luau::Position{ (unsigned)at.Line, (unsigned)at.Col };
	}

	Range LanguageService::Out(const std::string& name, const Luau::Location& at) const
	{
		Position begin{ (int)at.begin.line, (int)at.begin.column };
		Position end{ (int)at.end.line, (int)at.end.column };
		if (const TranslateResult* t = m_Analysis.Translation(name))
		{
			begin = t->Map.FromLuau(begin);
			end = t->Map.FromLuau(end);
		}
		return { begin, end };
	}

	// Checks the module for the editor's questions: every local's type
	// inferred, as Luau's autocomplete check does.
	bool LanguageService::Prepare(const std::string& name)
	{
		Luau::FrontendOptions options = m_Analysis.Frontend().options;
		options.forAutocomplete = true;
		m_Analysis.Frontend().check(name, options);
		return m_Analysis.Frontend().moduleResolverForAutocomplete.getModule(name) != nullptr;
	}

	CompletionList LanguageService::Complete(const std::string& path, int line, int col)
	{
		CompletionList list;
		const std::string name = Name(path);
		if (!Prepare(name))
			return list;
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!text)
			return list;
		const std::string lineText = LineOf(*text, line);
		col = std::clamp(col, 0, (int)lineText.size());
		int start = col;
		while (start > 0 && IsIdent(lineText[(size_t)start - 1]))
			start--;
		list.ReplaceFrom = start;
		const bool cluau = IsCluauPath(name);

		// The path in `require("...")`: the standard modules, and the
		// modules and directories beside the script (./name, ../name).
		if (const int from = RequirePathAt(lineText, col); from >= 0)
		{
			const std::string typed = lineText.substr((size_t)from, (size_t)(col - from));
			list.ReplaceFrom = from;
			for (const std::string& module : StdModuleNames())
				if (("@std/" + module).rfind(typed, 0) == 0)
					list.Items.push_back({ "@std/" + module, CompletionKind::Module, {}, {}, false });
			if (typed.rfind("./", 0) == 0 || typed.rfind("../", 0) == 0)
			{
				const size_t slash = typed.rfind('/');
				const std::string folder = typed.substr(0, slash + 1);
				std::error_code ec;
				const fs::path dir = fs::u8path(path).parent_path() / fs::u8path(folder);
				std::vector<std::string> found;
				for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
				{
					const fs::path& entry = it->path();
					const std::string file = PathText(entry.filename());
					if (file.empty() || file[0] == '.')
						continue;
					const std::string ext = PathText(entry.extension());
					std::error_code typeError;
					if (it->is_directory(typeError))
						found.push_back(folder + file + "/");
					else if ((ext == ".cluau" || ext == ".luau") && entry != fs::u8path(path))
						found.push_back(folder + PathText(entry.stem()));
				}
				std::sort(found.begin(), found.end());
				for (const std::string& candidate : found)
					if (candidate.rfind(typed, 0) == 0)
						list.Items.push_back({ candidate, CompletionKind::Module, {}, {}, false });
			}
			return list;
		}

		Luau::AutocompleteResult result = Luau::autocomplete(m_Analysis.Frontend(), name, In(name, line, col),
			[](std::string, std::optional<const Luau::ExternType*>, std::optional<std::string>) -> std::optional<Luau::AutocompleteEntryMap> {
				return std::nullopt;
			});

		std::vector<std::pair<int, CompletionItem>> ranked;
		std::set<std::string> seen;
		for (const auto& [label, entry] : result.entryMap)
		{
			if (entry.wrongIndexType || label == Luau::kGeneratedAnonymousFunctionEntryName || label.rfind("__", 0) == 0)
				continue;
			if (cluau && entry.kind == Luau::AutocompleteEntryKind::Keyword && IsLuauOnlyKeyword(label))
				continue;
			CompletionItem item;
			item.Label = label;
			item.Deprecated = entry.deprecated;
			const bool isFunction = entry.type && Luau::get<Luau::FunctionType>(Luau::follow(*entry.type));
			switch (entry.kind)
			{
			case Luau::AutocompleteEntryKind::Property: item.Kind = isFunction ? CompletionKind::Method : CompletionKind::Property; break;
			case Luau::AutocompleteEntryKind::Binding: item.Kind = isFunction ? CompletionKind::Function : CompletionKind::Variable; break;
			case Luau::AutocompleteEntryKind::Keyword: item.Kind = CompletionKind::Keyword; break;
			case Luau::AutocompleteEntryKind::Type: item.Kind = CompletionKind::Type; break;
			case Luau::AutocompleteEntryKind::Module:
			case Luau::AutocompleteEntryKind::RequirePath: item.Kind = CompletionKind::Module; break;
			default: item.Kind = CompletionKind::Value; break;
			}
			if (entry.type)
				item.Detail = TypeString(*entry.type);
			if (entry.insertText)
				item.InsertText = *entry.insertText;
			else if (entry.parens == Luau::ParenthesesRecommendation::CursorInside)
				item.InsertText = label + "($0)";
			else if (entry.parens == Luau::ParenthesesRecommendation::CursorAfter)
				item.InsertText = label + "()";
			const int rank = entry.typeCorrect != Luau::TypeCorrectKind::None ? 0 : item.Kind == CompletionKind::Keyword ? 2 : 1;
			seen.insert(label);
			ranked.push_back({ rank, std::move(item) });
		}
		// cluau's own statement words, where Luau offered its statements.
		if (cluau && result.entryMap.count("local"))
			for (const char* keyword : { "let", "const", "class", "enum", "try", "throw", "abstract", "switch" })
				if (seen.insert(keyword).second)
					ranked.push_back({ 2, { keyword, CompletionKind::Keyword, {}, {}, false } });
		std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
			return a.first != b.first ? a.first < b.first : a.second.Label < b.second.Label;
		});
		for (auto& entry : ranked)
			list.Items.push_back(std::move(entry.second));
		return list;
	}

	std::optional<HoverInfo> LanguageService::Hover(const std::string& path, int line, int col)
	{
		const std::string name = Name(path);
		if (!Prepare(name))
			return std::nullopt;
		Luau::Frontend& frontend = m_Analysis.Frontend();
		Luau::ModulePtr module = frontend.moduleResolverForAutocomplete.getModule(name);
		const Luau::SourceModule* source = frontend.getSourceModule(name);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!source || !module || !text)
			return std::nullopt;
		const Luau::Position pos = In(name, line, col);

		auto bindingType = [&](Luau::AstLocal* local) -> std::optional<Luau::TypeId> {
			for (const auto& [location, scope] : module->scopes)
				if (auto binding = scope->bindings.find(Luau::Symbol(local)); binding != scope->bindings.end())
					return binding->second.typeId;
			return std::nullopt;
		};
		auto declLine = [&](const Luau::Location& at) { return Out(name, at).Begin.Line; };
		// A class instance's type by the class's name.
		auto shown = [&](Luau::TypeId type) {
			const Luau::TypeId bare = StripOptional(type);
			if (std::optional<std::string> cls = m_Analysis.ClassNameOf(bare))
				return *cls + (bare != Luau::follow(type) ? "?" : "");
			return TypeString(type);
		};
		// A class, or one of its methods, as the class declares it.
		auto classText = [&](const std::string& moduleName, const std::string& symbol) -> std::optional<std::string> {
			if (const TranslateResult* t = m_Analysis.Translation(moduleName))
				for (const ClassInfo& c : t->Classes)
				{
					if (c.Name == symbol)
						return std::string(c.Abstract ? "abstract class " : "class ") + c.Name + (c.Base.empty() ? "" : " extends " + c.Base);
					for (const ClassMethod& m : c.Methods)
						if (m.Name == symbol)
							return c.Name + ":" + m.Name + "(" + m.Params + ")" + (m.ReturnType.empty() ? "" : ": " + m.ReturnType);
				}
			return std::nullopt;
		};

		HoverInfo info;
		// A declaration's name shows what a use of it shows: a class, its
		// methods and fields (as the class declares them), a function's
		// name, a local's, a parameter's.
		if (const TranslateResult* t = m_Analysis.Translation(name))
		{
			auto on = [&](const Range& r) { return r.Begin.Line == line && col >= r.Begin.Col && col <= r.End.Col; };
			for (const ClassInfo& c : t->Classes)
			{
				if (on(c.At))
				{
					info.Text = classText(name, c.Name).value_or("class " + c.Name);
					info.Documentation = DocAbove(*text, c.At.Begin.Line);
					info.At = c.At;
					return info;
				}
				for (const ClassMethod& m : c.Methods)
					if (on(m.At))
					{
						info.Text = c.Name + ":" + m.Name + "(" + m.Params + ")" + (m.ReturnType.empty() ? "" : ": " + m.ReturnType);
						info.Documentation = DocAbove(*text, m.At.Begin.Line);
						info.At = m.At;
						return info;
					}
				for (const ClassField& f : c.Fields)
					if (on(f.At))
					{
						info.Text = f.Name + (f.Type.empty() ? "" : ": " + f.Type);
						info.Documentation = DocAbove(*text, f.At.Begin.Line);
						info.At = f.At;
						return info;
					}
			}
		}
		for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, pos))
		{
			auto check = [&](Luau::AstLocal* local) -> bool {
				if (!local || !local->location.containsClosed(pos))
					return false;
				std::optional<Luau::TypeId> type = bindingType(local);
				info.Text = NameOf(local->name) + (type ? ": " + shown(*type) : "");
				info.Documentation = DocAbove(*text, declLine(local->location));
				info.At = Out(name, local->location);
				return true;
			};
			// `function Name(...)`, `function M.Name(...)`: the name and the function's type.
			if (Luau::AstStatFunction* fn = node->as<Luau::AstStatFunction>())
			{
				std::string symbol;
				Luau::Location at;
				if (Luau::AstExprGlobal* g = fn->name->as<Luau::AstExprGlobal>(); g && g->location.containsClosed(pos))
					symbol = NameOf(g->name), at = g->location;
				else if (Luau::AstExprIndexName* index = fn->name->as<Luau::AstExprIndexName>(); index && index->indexLocation.containsClosed(pos))
					symbol = NameOf(index->index), at = index->indexLocation;
				if (!symbol.empty())
				{
					const Luau::TypeId* type = module->astTypes.find(fn->func);
					info.Text = symbol + (type ? ": " + shown(*type) : "");
					info.Documentation = DocAbove(*text, declLine(at));
					info.At = Out(name, at);
					return info;
				}
			}
			if (Luau::AstStatLocal* decl = node->as<Luau::AstStatLocal>())
				for (Luau::AstLocal* local : decl->vars)
					if (check(local))
						return info;
			if (Luau::AstExprFunction* fn = node->as<Luau::AstExprFunction>())
				for (Luau::AstLocal* local : fn->args)
					if (check(local))
						return info;
			if (Luau::AstStatLocalFunction* fn = node->as<Luau::AstStatLocalFunction>(); fn && check(fn->name))
				return info;
			if (Luau::AstStatFor* loop = node->as<Luau::AstStatFor>(); loop && check(loop->var))
				return info;
			if (Luau::AstStatForIn* loop = node->as<Luau::AstStatForIn>())
				for (Luau::AstLocal* local : loop->vars)
					if (check(local))
						return info;
		}

		if (Luau::AstExpr* expr = Luau::findExprAtPosition(*source, pos))
		{
			const Luau::TypeId* type = module->astTypes.find(expr);
			info.At = Out(name, expr->location);
			if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
			{
				std::optional<Luau::TypeId> declared = bindingType(local->local);
				const Luau::TypeId localType = declared ? *declared : type ? *type : nullptr;
				const std::string localName = NameOf(local->local->name);
				if (std::optional<std::string> cls = classText(name, localName); cls && cls->rfind("class", 0) == 0)
					info.Text = *cls;
				else if (cls && cls->rfind("abstract", 0) == 0)
					info.Text = *cls;
				else
					info.Text = localName + (localType ? ": " + shown(localType) : "");
				info.Documentation = DocAbove(*text, declLine(local->local->location));
				return info;
			}
			if (!type)
				return std::nullopt;
			if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
			{
				info.Text = NameOf(global->name) + ": " + shown(*type);
				// A script's own function: the comment above it.
				struct Finder : Luau::AstVisitor
				{
					std::string Name;
					std::optional<Luau::Location> Found;
					bool visit(Luau::AstStatFunction* fn) override
					{
						if (Luau::AstExprGlobal* g = fn->name->as<Luau::AstExprGlobal>(); g && !Found && NameOf(g->name) == Name)
							Found = g->location;
						return false;
					}
				} finder;
				finder.Name = NameOf(global->name);
				source->root->visit(&finder);
				if (finder.Found)
					info.Documentation = DocAbove(*text, declLine(*finder.Found));
			}
			else if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
			{
				const std::string member = NameOf(index->index);
				info.Text = member + ": " + shown(*type);
				info.At = Out(name, index->indexLocation);
				// A member declared in a module: as its class says it, with
				// the comment above it there.
				if (const Luau::TypeId* base = module->astTypes.find(index->expr))
					if (Member m = FindMember(*base, member); m.Prop && !m.Module.empty())
					{
						if (std::optional<std::string> cls = classText(m.Module, member))
							info.Text = *cls;
						std::optional<Luau::Location> at = m.Prop->location ? m.Prop->location : m.Prop->typeLocation;
						std::shared_ptr<const SourceText> declared = SourceOf(m.Module);
						if (at && declared)
						{
							Position p{ (int)at->begin.line, (int)at->begin.column };
							if (const TranslateResult* t = m_Analysis.Translation(m.Module))
							{
								p = t->Map.FromLuau(p);
								for (const ClassInfo& c : t->Classes)
								{
									for (const ClassMethod& cm : c.Methods)
										if (cm.Name == member)
											p = cm.At.Begin;
									for (const ClassField& f : c.Fields)
										if (f.Name == member)
											p = f.At.Begin;
									if (c.Name == member)
										p = c.At.Begin;
								}
							}
							info.Documentation = DocAbove(*declared, p.Line);
						}
					}
			}
			else if (Luau::AstExprConstantString* key = expr->as<Luau::AstExprConstantString>())
			{
				// A field's name in a table (an enum's member): the field and its value's type.
				for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, pos))
					if (Luau::AstExprTable* table = node->as<Luau::AstExprTable>())
						for (const Luau::AstExprTable::Item& item : table->items)
							if (item.kind == Luau::AstExprTable::Item::Kind::Record && item.key == key)
							{
								const Luau::TypeId* value = module->astTypes.find(item.value);
								info.Text = std::string(key->value.data, key->value.size) + (value ? ": " + shown(*value) : "");
								info.Documentation = DocAbove(*text, declLine(key->location));
								return info;
							}
				return std::nullopt;
			}
			else if (expr->is<Luau::AstExprConstantNumber>() || expr->is<Luau::AstExprConstantBool>())
				return std::nullopt;
			else
				info.Text = shown(*type);
			return info;
		}

		return std::nullopt;
	}

	std::optional<SignatureInfo> LanguageService::Signature(const std::string& path, int line, int col)
	{
		const std::string name = Name(path);
		if (!Prepare(name))
			return std::nullopt;
		Luau::Frontend& frontend = m_Analysis.Frontend();
		Luau::ModulePtr module = frontend.moduleResolverForAutocomplete.getModule(name);
		const Luau::SourceModule* source = frontend.getSourceModule(name);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!source || !module || !text)
			return std::nullopt;

		// The unmatched '(' before the caret, and the top-level commas since.
		int openLine = -1, openCol = -1, commas = 0, depth = 0;
		for (int l = line; l >= 0 && l > line - 24 && openLine < 0; l--)
		{
			const std::string lineText = LineOf(*text, l);
			int c = l == line ? std::min(col, (int)lineText.size()) : (int)lineText.size();
			for (c = c - 1; c >= 0; c--)
			{
				const char ch = lineText[(size_t)c];
				if (ch == ')' || ch == '}' || ch == ']')
					depth++;
				else if (ch == '{' || ch == '[')
				{
					if (depth == 0)
						return std::nullopt;
					depth--;
				}
				else if (ch == '(')
				{
					if (depth == 0)
					{
						openLine = l;
						openCol = c;
						break;
					}
					depth--;
				}
				else if (ch == ',' && depth == 0)
					commas++;
			}
		}
		if (openLine < 0)
			return std::nullopt;

		// The call whose argument list opens there.
		const Luau::Position open = In(name, openLine, openCol);
		const Luau::Position inside{ open.line, open.column + 1 };
		Luau::AstExprCall* call = nullptr;
		for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, inside))
			if (Luau::AstExprCall* candidate = node->as<Luau::AstExprCall>())
				if (candidate->argLocation.begin == open || candidate->argLocation.begin == inside)
					call = candidate;
		if (!call)
			return std::nullopt;
		const Luau::TypeId* calleeType = module->astTypes.find(call->func);
		if (!calleeType)
			return std::nullopt;
		Luau::TypeId callee = Luau::follow(*calleeType);
		if (const Luau::IntersectionType* overloads = Luau::get<Luau::IntersectionType>(callee); overloads && !overloads->parts.empty())
			callee = Luau::follow(overloads->parts[0]);
		// A class called to construct: its __call.
		if (const Luau::MetatableType* meta = Luau::get<Luau::MetatableType>(callee))
			if (const Luau::TableType* mt = Luau::get<Luau::TableType>(Luau::follow(meta->metatable)))
				if (auto call = mt->props.find("__call"); call != mt->props.end() && call->second.readTy)
					callee = Luau::follow(*call->second.readTy);
		const Luau::FunctionType* fn = Luau::get<Luau::FunctionType>(callee);
		if (!fn)
			return std::nullopt;

		std::string calleeName = "function";
		if (Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>())
			calleeName = NameOf(index->index);
		else if (Luau::AstExprGlobal* global = call->func->as<Luau::AstExprGlobal>())
			calleeName = NameOf(global->name);
		else if (Luau::AstExprLocal* local = call->func->as<Luau::AstExprLocal>())
			calleeName = NameOf(local->local->name);

		SignatureInfo info;
		info.Label = calleeName + "(";
		const auto [params, tail] = Luau::flatten(fn->argTypes);
		// A method call, or a constructor's class argument, takes the first
		// parameter itself.
		const bool constructor = callee != Luau::follow(*calleeType) && !Luau::get<Luau::IntersectionType>(Luau::follow(*calleeType));
		const size_t skip = (call->self || constructor) && !params.empty() ? 1 : 0;
		for (size_t i = skip; i < params.size(); i++)
		{
			if (i > skip)
				info.Label += ", ";
			const int begin = (int)info.Label.size();
			const std::string paramName =
				i < fn->argNames.size() && fn->argNames[i] ? fn->argNames[i]->name : "arg" + std::to_string(i + 1 - skip);
			info.Label += paramName + ": " + TypeString(params[i]);
			info.Params.push_back({ begin, (int)info.Label.size() });
		}
		if (tail)
		{
			if (params.size() > skip)
				info.Label += ", ";
			const int begin = (int)info.Label.size();
			info.Label += "...";
			if (const Luau::VariadicTypePack* rest = Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail)))
				info.Label += ": " + TypeString(rest->ty);
			info.Params.push_back({ begin, (int)info.Label.size() });
		}
		info.Label += ")";
		const std::string returns = TypePackString(fn->retTypes);
		if (returns != "()")
			info.Label += ": " + returns;
		if (!info.Params.empty())
			info.Active = std::min(commas, (int)info.Params.size() - 1);
		return info;
	}

	std::string LanguageService::PathOf(const std::string& name) const
	{
		auto open = m_Open.find(name);
		return open == m_Open.end() ? name : open->second;
	}

	std::optional<Location> LanguageService::Definition(const std::string& path, int line, int col)
	{
		std::optional<Location> found = Find(path, line, col);
		// An open file by the path the editor knows it by (a module's name is
		// its canonical path, which may differ: a link, a short name).
		if (found)
			found->Path = PathOf(found->Path);
		return found;
	}

	std::optional<Location> LanguageService::Mapped(const std::string& moduleName, const Luau::Location& at, const std::string& symbol) const
	{
		const std::shared_ptr<const SourceText> text = SourceOf(moduleName);
		if (!text)
			return std::nullopt;
		const Range r = Out(moduleName, at);
		if (IdentifierAt(*text, r.Begin.Line, r.Begin.Col, symbol))
			return Location{ moduleName, { r.Begin, { r.Begin.Line, r.Begin.Col + (int)symbol.size() } } };
		// A class member: where the class declares it (its generated line
		// does not hold the name).
		if (const TranslateResult* t = m_Analysis.Translation(moduleName))
			for (const ClassInfo& c : t->Classes)
			{
				if (c.Name == symbol)
					return Location{ moduleName, c.At };
				for (const ClassMethod& m : c.Methods)
					if (m.Name == symbol)
						return Location{ moduleName, m.At };
				for (const ClassField& f : c.Fields)
					if (f.Name == symbol)
						return Location{ moduleName, f.At };
			}
		return std::nullopt;
	}

	// Needs the module checked for autocomplete (Prepare): the expression's type.
	std::optional<Location> LanguageService::MemberDeclaration(const std::string& name, const Luau::AstExprIndexName* index)
	{
		Luau::ModulePtr module = m_Analysis.Frontend().moduleResolverForAutocomplete.getModule(name);
		if (!module)
			return std::nullopt;
		const std::string member = NameOf(index->index);
		if (const Luau::TypeId* type = module->astTypes.find(index->expr))
			if (Member m = FindMember(*type, member); m.Prop && !m.Module.empty())
			{
				if (m.Prop->location)
					if (std::optional<Location> found = Mapped(m.Module, *m.Prop->location, member))
						return found;
				if (m.Prop->typeLocation)
					if (std::optional<Location> found = Mapped(m.Module, *m.Prop->typeLocation, member))
						return found;
			}
		return std::nullopt;
	}

	std::optional<Location> LanguageService::Find(const std::string& path, int line, int col)
	{
		const std::string name = Name(path);
		if (!Prepare(name))
			return std::nullopt;
		Luau::Frontend& frontend = m_Analysis.Frontend();
		Luau::ModulePtr module = frontend.moduleResolverForAutocomplete.getModule(name);
		const Luau::SourceModule* source = frontend.getSourceModule(name);
		if (!source || !module)
			return std::nullopt;
		const Luau::Position pos = In(name, line, col);
		auto mapped = [&](const std::string& moduleName, const Luau::Location& at, const std::string& symbol) { return Mapped(moduleName, at, symbol); };

		const std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, pos);
		for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
		{
			Luau::AstNode* node = *it;
			if (Luau::AstExprLocal* local = node->as<Luau::AstExprLocal>())
				return mapped(name, local->local->location, NameOf(local->local->name));
			if (Luau::AstExprGlobal* global = node->as<Luau::AstExprGlobal>())
			{
				// A script's top-level function (a global in script mode), or
				// the first assignment to the global.
				struct Finder : Luau::AstVisitor
				{
					std::string Name;
					std::optional<Luau::Location> Found;
					bool visit(Luau::AstStatFunction* fn) override
					{
						if (Luau::AstExprGlobal* g = fn->name->as<Luau::AstExprGlobal>(); g && !Found && NameOf(g->name) == Name)
							Found = g->location;
						return true;
					}
					bool visit(Luau::AstStatAssign* assign) override
					{
						for (Luau::AstExpr* target : assign->vars)
							if (Luau::AstExprGlobal* g = target->as<Luau::AstExprGlobal>(); g && !Found && NameOf(g->name) == Name)
								Found = g->location;
						return true;
					}
				} finder;
				finder.Name = NameOf(global->name);
				source->root->visit(&finder);
				return finder.Found ? mapped(name, *finder.Found, finder.Name) : std::nullopt;
			}
			if (Luau::AstExprIndexName* index = node->as<Luau::AstExprIndexName>())
			{
				if (!index->indexLocation.containsClosed(pos))
					continue;
				return MemberDeclaration(name, index);
			}
			if (Luau::AstExprConstantString* string = node->as<Luau::AstExprConstantString>())
			{
				// A require's module.
				if (it + 1 != ancestry.rend())
					if (Luau::AstExprCall* call = (*(it + 1))->as<Luau::AstExprCall>())
						if (Luau::AstExprGlobal* fn = call->func->as<Luau::AstExprGlobal>(); fn && NameOf(fn->name) == "require")
						{
							const std::string request(string->value.data, string->value.size);
							std::optional<std::string> target = m_Analysis.Resolve(name, request);
							if (target && (*target)[0] != '@')
								return Location{ *target, {} };
						}
				return std::nullopt;
			}
		}
		return std::nullopt;
	}

	void LanguageService::SetWorkspace(std::vector<std::string> folders) { m_Workspace = std::move(folders); }

	std::vector<std::string> LanguageService::SearchFiles() const
	{
		std::vector<std::string> out;
		std::set<std::string> seen;
		auto add = [&](const std::string& path) {
			if (seen.insert(Name(path)).second)
				out.push_back(path);
		};
		for (const auto& [name, path] : m_Open)
			add(path);
		for (const std::string& folder : m_Workspace)
		{
			std::error_code ec;
			fs::recursive_directory_iterator it(fs::u8path(folder), fs::directory_options::skip_permission_denied, ec), end;
			for (; !ec && it != end && out.size() < 5000; it.increment(ec))
			{
				const fs::path& p = it->path();
				const std::string leaf = PathText(p.filename());
				if (it->is_directory(ec))
				{
					// Packages, dependencies and build trees are not the workspace's own.
					if (leaf.empty() || leaf[0] == '.' || leaf == "node_modules" || leaf == "cluau_modules" || leaf == "_deps")
						it.disable_recursion_pending();
					continue;
				}
				const std::string ext = PathText(p.extension());
				if (ext == ".cluau" || ext == ".luau")
					add(PathText(p));
			}
		}
		return out;
	}

	std::vector<Location> LanguageService::References(const std::string& path, int line, int col, bool* declared)
	{
		std::string declaredIn;
		bool first = false;
		std::vector<Location> out = ReferencesOf(path, line, col, declaredIn, first);
		if (declared)
			*declared = first;
		return out;
	}

	std::vector<Location> LanguageService::ReferencesOf(const std::string& path, int line, int col, std::string& declaredIn, bool& declared)
	{
		declared = false;
		std::vector<Location> out;
		const std::string name = Name(path);
		if (!Prepare(name))
			return out;
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(name);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!source || !source->root || !text)
			return out;
		const Luau::Position pos = In(name, line, col);
		std::set<std::pair<std::string, std::pair<int, int>>> seen;
		// Only where the text holds the name (not a translation's own words).
		auto add = [&](const std::string& module, const std::string& file, const Range& r, const std::string& symbol) {
			const std::shared_ptr<const SourceText> moduleText = SourceOf(module);
			if (!moduleText || !IdentifierAt(*moduleText, r.Begin.Line, r.Begin.Col, symbol) || !seen.insert({ module, { r.Begin.Line, r.Begin.Col } }).second)
				return false;
			out.push_back({ file, { r.Begin, { r.Begin.Line, r.Begin.Col + (int)symbol.size() } } });
			return true;
		};

		Luau::AstLocal* target = nullptr;
		Luau::AstExprGlobal* global = nullptr;
		Luau::AstExprIndexName* member = nullptr;
		for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, pos))
		{
			auto declared = [&](Luau::AstLocal* local) {
				if (local && local->location.containsClosed(pos))
					target = local;
			};
			if (Luau::AstExprLocal* local = node->as<Luau::AstExprLocal>())
				target = local->local;
			else if (Luau::AstExprGlobal* g = node->as<Luau::AstExprGlobal>())
				global = g;
			else if (Luau::AstExprIndexName* index = node->as<Luau::AstExprIndexName>(); index && index->indexLocation.containsClosed(pos))
				member = index;
			else if (Luau::AstStatLocal* decl = node->as<Luau::AstStatLocal>())
				for (Luau::AstLocal* local : decl->vars)
					declared(local);
			else if (Luau::AstExprFunction* fn = node->as<Luau::AstExprFunction>())
				for (Luau::AstLocal* local : fn->args)
					declared(local);
			else if (Luau::AstStatLocalFunction* fn = node->as<Luau::AstStatLocalFunction>())
				declared(fn->name);
			else if (Luau::AstStatFor* loop = node->as<Luau::AstStatFor>())
				declared(loop->var);
			else if (Luau::AstStatForIn* loop = node->as<Luau::AstStatForIn>())
				for (Luau::AstLocal* local : loop->vars)
					declared(local);
		}

		// A class member's declaration in a class body.
		std::optional<Location> declaration;
		std::string symbol;
		if (const TranslateResult* t = m_Analysis.Translation(name))
			for (const ClassInfo& c : t->Classes)
			{
				auto at = [&](const std::string& n, const Range& r) {
					if (!declaration && r.Begin.Line == line && col >= r.Begin.Col && col <= r.End.Col)
					{
						declaration = Location{ name, r };
						symbol = n;
					}
				};
				for (const ClassMethod& m : c.Methods)
					at(m.Name, m.At);
				for (const ClassField& f : c.Fields)
					at(f.Name, f.At);
			}

		if (!declaration && member)
		{
			declaration = MemberDeclaration(name, member);
			symbol = NameOf(member->index);
		}
		if (!declaration && target)
		{
			symbol = NameOf(target->name);
			LocalUses uses;
			uses.Target = target;
			source->root->visit(&uses);
			declared = add(name, path, Out(name, target->location), symbol);
			for (const Luau::Location& at : uses.Found)
				add(name, path, Out(name, at), symbol);
			return out;
		}
		if (!declaration && global)
		{
			// A script's top-level function: a global of its file, declared
			// where its function statement (or the first assignment) names it.
			symbol = NameOf(global->name);
			struct Globals : Luau::AstVisitor
			{
				std::string Name;
				std::vector<Luau::Location> Found;
				std::optional<Luau::Location> Declared;
				bool visit(Luau::AstStatFunction* fn) override
				{
					if (Luau::AstExprGlobal* g = fn->name->as<Luau::AstExprGlobal>(); g && !Declared && NameOf(g->name) == Name)
						Declared = g->location;
					return true;
				}
				bool visit(Luau::AstStatAssign* assign) override
				{
					for (Luau::AstExpr* target : assign->vars)
						if (Luau::AstExprGlobal* g = target->as<Luau::AstExprGlobal>(); g && !Declared && NameOf(g->name) == Name)
							Declared = g->location;
					return true;
				}
				bool visit(Luau::AstExprGlobal* g) override
				{
					if (NameOf(g->name) == Name)
						Found.push_back(g->location);
					return true;
				}
			} globals;
			globals.Name = symbol;
			source->root->visit(&globals);
			// One the script does not declare is the runtime's (print, string).
			if (globals.Declared)
				declared = add(name, path, Out(name, *globals.Declared), symbol);
			else
				declaredIn = kRuntimeGlobal;
			for (const Luau::Location& at : globals.Found)
				add(name, path, Out(name, at), symbol);
			return out;
		}
		if (!declaration)
			return out;

		// A member: its declaration, then every use in the workspace's files
		// that resolves to it.
		const Location decl = *declaration;
		declaredIn = decl.Path;
		if (!decl.Path.empty() && decl.Path[0] != '@')
			declared = add(decl.Path, PathOf(decl.Path), decl.At, symbol);
		struct Members : Luau::AstVisitor
		{
			std::string Name;
			std::vector<Luau::AstExprIndexName*> Found;
			bool visit(Luau::AstExprIndexName* index) override
			{
				if (NameOf(index->index) == Name)
					Found.push_back(index);
				return true;
			}
		};
		for (const std::string& file : SearchFiles())
		{
			const std::string module = Name(file);
			const std::shared_ptr<const SourceText> fileText = SourceOf(module);
			if (!fileText || fileText->Text.find(symbol) == std::string::npos)
				continue;
			// A file nothing has required yet is checked as a script.
			const std::vector<std::string>& known = m_Analysis.UserModules();
			if (!m_Open.count(module) && std::find(known.begin(), known.end(), module) == known.end())
				m_Analysis.AddRoot(fs::u8path(file));
			if (!Prepare(module))
				continue;
			const Luau::SourceModule* other = m_Analysis.Frontend().getSourceModule(module);
			if (!other || !other->root)
				continue;
			Members members;
			members.Name = symbol;
			other->root->visit(&members);
			for (Luau::AstExprIndexName* index : members.Found)
			{
				const std::optional<Location> resolved = MemberDeclaration(module, index);
				if (resolved && resolved->Path == decl.Path && resolved->At.Begin == decl.At.Begin)
					add(module, PathOf(module) == module ? file : PathOf(module), Out(module, index->indexLocation), symbol);
			}
		}
		return out;
	}

	std::map<std::string, std::vector<TextEdit>> LanguageService::Rename(const std::string& path, int line, int col, const std::string& newName, std::string& error)
	{
		std::map<std::string, std::vector<TextEdit>> edits;
		if (!IsIdentifier(newName) || IsKeyword(newName))
		{
			error = "'" + newName + "' is not a name a variable can have";
			return edits;
		}
		std::string declaredIn;
		bool declared = false;
		const std::vector<Location> uses = ReferencesOf(path, line, col, declaredIn, declared);
		if (declaredIn == kRuntimeGlobal)
		{
			const std::shared_ptr<const SourceText> text = Source(path);
			std::string symbol = "this name";
			if (!uses.empty() && text)
			{
				const Location& use = uses.front();
				symbol = "'" + std::string(text->Line(use.At.Begin.Line).substr((size_t)use.At.Begin.Col, (size_t)(use.At.End.Col - use.At.Begin.Col))) + "'";
			}
			error = symbol + " is a global this script does not declare (one of the runtime's): it cannot be renamed here";
			return edits;
		}
		if (!declaredIn.empty() && declaredIn[0] == '@')
		{
			error = "'" + declaredIn + "' declares this name: it cannot be renamed here";
			return edits;
		}
		if (uses.empty())
		{
			error = "only a variable, a function, a parameter or a member of a class or module can be renamed here";
			return edits;
		}
		for (const Location& use : uses)
			edits[use.Path].push_back({ use.At, newName });
		return edits;
	}

	std::vector<DocumentSymbol> LanguageService::DocumentSymbols(const std::string& path)
	{
		std::vector<DocumentSymbol> out;
		const std::string name = Name(path);
		m_Analysis.Frontend().check(name);
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(name);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!source || !source->root || !text)
			return out;
		const TranslateResult* translation = m_Analysis.Translation(name);
		std::set<std::string> classNames;
		if (translation)
			for (const ClassInfo& c : translation->Classes)
			{
				DocumentSymbol cls;
				cls.Name = c.Name;
				cls.Kind = SymbolKind::Class;
				cls.Detail = c.Base.empty() ? "" : "extends " + c.Base;
				cls.At = c.At;
				cls.Extent = { { c.At.Begin.Line, 0 }, { c.BodyEndLine, (int)LineOf(*text, c.BodyEndLine).size() } };
				for (const ClassField& f : c.Fields)
					cls.Children.push_back({ f.Name, SymbolKind::Field, f.Type, f.At, f.At, {} });
				for (const ClassMethod& m : c.Methods)
				{
					DocumentSymbol method{ m.Name, SymbolKind::Method, "(" + m.Params + ")" + (m.ReturnType.empty() ? "" : ": " + m.ReturnType), m.At,
						{ { m.At.Begin.Line, 0 }, { m.BodyEndLine, (int)LineOf(*text, m.BodyEndLine).size() } }, {} };
					cls.Children.push_back(std::move(method));
				}
				classNames.insert(c.Name);
				out.push_back(std::move(cls));
			}
		// The file's top-level functions and variables.
		auto add = [&](const std::string& symbol, SymbolKind kind, const Luau::Location& nameAt, const Luau::Location& extent) {
			if (symbol.empty() || symbol.rfind("__", 0) == 0 || classNames.count(symbol))
				return;
			const Range at = Out(name, nameAt);
			if (!IdentifierAt(*text, at.Begin.Line, at.Begin.Col, symbol))
				return;
			Range whole = Out(name, extent);
			whole.Begin = { at.Begin.Line, 0 };
			out.push_back({ symbol, kind, {}, { at.Begin, { at.Begin.Line, at.Begin.Col + (int)symbol.size() } }, whole, {} });
		};
		for (Luau::AstStat* stat : source->root->body)
		{
			if (Luau::AstStatLocalFunction* fn = stat->as<Luau::AstStatLocalFunction>())
				add(NameOf(fn->name->name), SymbolKind::Function, fn->name->location, fn->location);
			else if (Luau::AstStatFunction* fn = stat->as<Luau::AstStatFunction>())
			{
				if (Luau::AstExprGlobal* global = fn->name->as<Luau::AstExprGlobal>())
					add(NameOf(global->name), SymbolKind::Function, global->location, fn->location);
			}
			else if (Luau::AstStatLocal* decl = stat->as<Luau::AstStatLocal>())
				for (size_t i = 0; i < decl->vars.size; i++)
				{
					Luau::AstLocal* var = decl->vars.data[i];
					const bool function = i < decl->values.size && decl->values.data[i]->is<Luau::AstExprFunction>();
					add(NameOf(var->name), function ? SymbolKind::Function : SymbolKind::Variable, var->location, decl->location);
				}
		}
		std::sort(out.begin(), out.end(), [](const DocumentSymbol& a, const DocumentSymbol& b) { return a.At.Begin < b.At.Begin; });
		return out;
	}

	namespace {

		// The query's letters appear in the name in order (any case).
		bool Matches(const std::string& name, const std::string& query)
		{
			size_t at = 0;
			for (char c : name)
				if (at < query.size() && std::tolower((unsigned char)c) == std::tolower((unsigned char)query[at]))
					at++;
			return at == query.size();
		}

	} // namespace

	std::vector<WorkspaceSymbol> LanguageService::WorkspaceSymbols(const std::string& query)
	{
		std::vector<WorkspaceSymbol> out;
		for (const std::string& file : SearchFiles())
		{
			const std::string module = Name(file);
			const std::vector<std::string>& known = m_Analysis.UserModules();
			if (!m_Open.count(module) && std::find(known.begin(), known.end(), module) == known.end())
				m_Analysis.AddRoot(fs::u8path(file));
			for (const DocumentSymbol& symbol : DocumentSymbols(file))
			{
				if (Matches(symbol.Name, query))
					out.push_back({ symbol.Name, symbol.Kind, {}, { file, symbol.At } });
				for (const DocumentSymbol& child : symbol.Children)
					if (Matches(child.Name, query))
						out.push_back({ child.Name, child.Kind, symbol.Name, { file, child.At } });
			}
			if (out.size() >= 1000)
				break;
		}
		return out;
	}

	std::vector<InlayHint> LanguageService::InlayHints(const std::string& path, int fromLine, int toLine)
	{
		std::vector<InlayHint> out;
		const std::string name = Name(path);
		if (!IsCluauPath(name) || !Prepare(name))
			return out;
		Luau::ModulePtr module = m_Analysis.Frontend().moduleResolverForAutocomplete.getModule(name);
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(name);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!module || !source || !source->root || !text)
			return out;

		struct Declarations : Luau::AstVisitor
		{
			// A name, and where its scope can be looked up.
			std::vector<std::pair<Luau::AstLocal*, Luau::Position>> Found;
			bool visit(Luau::AstStatLocal* decl) override
			{
				for (size_t i = 0; i < decl->vars.size; i++)
				{
					Luau::AstLocal* var = decl->vars.data[i];
					// What the line says already: an annotation, a function,
					// or a literal's own type.
					Luau::AstExpr* value = i < decl->values.size ? decl->values.data[i] : nullptr;
					if (var->annotation || !value || value->is<Luau::AstExprFunction>() || value->is<Luau::AstExprConstantNumber>() ||
						value->is<Luau::AstExprConstantString>() || value->is<Luau::AstExprConstantBool>() || value->is<Luau::AstExprConstantNil>() ||
						value->is<Luau::AstExprInterpString>())
						continue;
					Found.push_back({ var, decl->location.end });
				}
				return true;
			}
			bool visit(Luau::AstStatForIn* loop) override
			{
				for (Luau::AstLocal* var : loop->vars)
					if (!var->annotation)
						Found.push_back({ var, loop->body->location.begin });
				return true;
			}
		} declarations;
		source->root->visit(&declarations);

		for (const auto& [var, scopeAt] : declarations.Found)
		{
			const std::string symbol = NameOf(var->name);
			if (symbol.empty() || symbol == "_" || symbol.rfind("__", 0) == 0)
				continue;
			const Range at = Out(name, var->location);
			if (at.Begin.Line < fromLine || at.Begin.Line > toLine || !IdentifierAt(*text, at.Begin.Line, at.Begin.Col, symbol))
				continue;
			// A class or an enum names itself: the local the translation
			// declares for it has no hint.
			const std::string before = WordBefore(LineOf(*text, at.Begin.Line), at.Begin.Col);
			if (before == "class" || before == "enum")
				continue;
			Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, scopeAt);
			std::optional<Luau::TypeId> type;
			for (Luau::ScopePtr s = scope; s && !type; s = s->parent)
				if (auto binding = s->bindings.find(Luau::Symbol(var)); binding != s->bindings.end())
					type = binding->second.typeId;
			if (!type)
				continue;
			std::string label;
			if (std::optional<std::string> cls = m_Analysis.ClassNameOf(*type))
				label = *cls;
			else
				label = TypeString(*type);
			if (label.empty() || label.find("error-type") != std::string::npos || label == "any" || label == "unknown" || label == "nil")
				continue;
			if (label.size() > 48)
				label = label.substr(0, 45) + "...";
			out.push_back({ { at.Begin.Line, at.Begin.Col + (int)symbol.size() }, ": " + label });
		}
		std::sort(out.begin(), out.end(), [](const InlayHint& a, const InlayHint& b) { return a.At < b.At; });
		return out;
	}

	// ---- Quick fixes ------------------------------------------------------------

	namespace {

		// The edits that turn one name into another (the cost of typos).
		size_t Distance(const std::string& a, const std::string& b)
		{
			std::vector<size_t> row(b.size() + 1);
			for (size_t j = 0; j <= b.size(); j++)
				row[j] = j;
			for (size_t i = 1; i <= a.size(); i++)
			{
				size_t diagonal = row[0];
				row[0] = i;
				for (size_t j = 1; j <= b.size(); j++)
				{
					const size_t above = row[j];
					const bool same = std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 1]);
					row[j] = std::min({ row[j] + 1, row[j - 1] + 1, diagonal + (same ? 0 : 1) });
					diagonal = above;
				}
			}
			return row[b.size()];
		}

		// The name a diagnostic is about: the first quoted one after `after`.
		std::string QuotedAfter(const std::string& message, const char* after)
		{
			const size_t at = message.find(after);
			if (at == std::string::npos)
				return {};
			const size_t open = message.find('\'', at + std::strlen(after) - 1);
			if (open == std::string::npos)
				return {};
			const size_t close = message.find('\'', open + 1);
			return close == std::string::npos ? std::string() : message.substr(open + 1, close - open - 1);
		}

		// Where the line's `//` comment starts (-1 for none): outside its
		// strings and block comments.
		int LineCommentAt(const std::string& line)
		{
			char quote = 0;
			for (size_t i = 0; i < line.size(); i++)
			{
				const char c = line[i];
				if (quote)
				{
					if (c == '\\')
						i++;
					else if (c == quote)
						quote = 0;
				}
				else if (c == '"' || c == '\'' || c == '`')
					quote = c;
				else if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
				{
					const size_t close = line.find("*/", i + 2);
					if (close == std::string::npos)
						return -1;
					i = close + 1;
				}
				else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
					return (int)i;
			}
			return -1;
		}


	} // namespace

	std::vector<CodeAction> LanguageService::CodeActions(const std::string& path, const std::vector<DiagnosticRef>& diagnostics)
	{
		std::vector<CodeAction> out;
		const std::string name = Name(path);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!text)
			return out;
		for (const DiagnosticRef& d : diagnostics)
		{
			const std::string lineText = LineOf(*text, d.At.Begin.Line);
			if (d.Code == "NoAny")
			{
				// The line's exemption, at its end; a line with a comment
				// already has the exemption start it (an exemption is a line
				// comment of its own, and a line has one).
				CodeAction action;
				action.Title = "Allow any on this line (// allow-any:)";
				const int comment = LineCommentAt(lineText);
				if (comment < 0)
					action.Edits[path].push_back({ { { d.At.Begin.Line, (int)lineText.size() }, { d.At.Begin.Line, (int)lineText.size() } }, " // allow-any: reason" });
				else
				{
					int text = comment + 2;
					while (text < (int)lineText.size() && (lineText[(size_t)text] == ' ' || lineText[(size_t)text] == '\t'))
						text++;
					action.Edits[path].push_back({ { { d.At.Begin.Line, comment }, { d.At.Begin.Line, text } }, "// allow-any: reason - "});
				}
				out.push_back(std::move(action));
				continue;
			}
			std::string wrong = QuotedAfter(d.Message, "Unknown global '");
			const bool global = !wrong.empty();
			if (wrong.empty())
				wrong = QuotedAfter(d.Message, "Key '");
			if (wrong.empty())
				wrong = QuotedAfter(d.Message, "does not have key '");
			if (wrong.empty())
				continue;
			// Where the name is on the line (the diagnostic may cover more).
			int col = d.At.Begin.Col;
			if (!IdentifierAt(*text, d.At.Begin.Line, col, wrong))
			{
				const size_t found = lineText.find(wrong, (size_t)std::max(0, d.At.Begin.Col));
				if (found == std::string::npos)
					continue;
				col = (int)found;
			}
			const Range at{ { d.At.Begin.Line, col }, { d.At.Begin.Line, col + (int)wrong.size() } };
			if (global)
				for (const std::string& module : StdModuleNames())
					if (wrong == module)
					{
						// After the shebang and the requires the file starts with.
						int line = 0;
						for (;; line++)
						{
							const std::string l = LineOf(*text, line);
							if (l.rfind("#!", 0) == 0 || (l.find("require(") != std::string::npos && l.rfind("const ", 0) == 0))
								continue;
							break;
						}
						CodeAction action;
						action.Title = "Add const " + module + " = require(\"@std/" + module + "\")";
						action.Edits[path].push_back({ { { line, 0 }, { line, 0 } }, "const " + module + " = require(\"@std/" + module + "\")\n" });
						action.Preferred = true;
						out.push_back(std::move(action));
					}
			// The names that could stand there, nearest first.
			const CompletionList names = Complete(path, at.End.Line, at.End.Col);
			std::vector<std::pair<size_t, std::string>> near;
			const size_t limit = std::max<size_t>(1, wrong.size() / 3);
			for (const CompletionItem& item : names.Items)
			{
				if (item.Kind == CompletionKind::Keyword || item.Label == wrong)
					continue;
				const size_t distance = Distance(wrong, item.Label);
				if (distance <= limit)
					near.push_back({ distance, item.Label });
			}
			std::sort(near.begin(), near.end());
			for (size_t i = 0; i < near.size() && i < 3; i++)
			{
				CodeAction action;
				action.Title = "Change to '" + near[i].second + "'";
				action.Edits[path].push_back({ at, near[i].second });
				action.Preferred = i == 0 && near[i].first == 1;
				out.push_back(std::move(action));
			}
		}
		return out;
	}

	// ---- Semantic tokens --------------------------------------------------------

	std::vector<SemanticToken> LanguageService::SemanticTokens(const std::string& path)
	{
		std::vector<SemanticToken> out;
		const std::string name = Name(path);
		if (!Prepare(name))
			return out;
		Luau::ModulePtr module = m_Analysis.Frontend().moduleResolverForAutocomplete.getModule(name);
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(name);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!module || !source || !source->root || !text)
			return out;
		std::set<std::string> classes;
		const TranslateResult* translation = m_Analysis.Translation(name);
		if (translation)
			for (const ClassInfo& c : translation->Classes)
				classes.insert(c.Name);

		struct Names : Luau::AstVisitor
		{
			struct Found
			{
				Luau::Location At;
				std::string Name;
				TokenType Type;
				unsigned Modifiers;
			};
			std::vector<Found> Items;
			std::set<Luau::AstLocal*> Parameters;
			std::set<std::string> FileFunctions; // the script's own top-level functions
			const Luau::Module* Checked = nullptr;
			const std::set<std::string>* Classes = nullptr;

			bool IsFunction(Luau::AstExpr* e) const
			{
				const Luau::TypeId* t = Checked->astTypes.find(e);
				return t && Luau::get<Luau::FunctionType>(Luau::follow(*t));
			}
			// A local's name: where it is declared, or `at` (a use).
			void Local(Luau::AstLocal* local, unsigned modifiers, bool function, const Luau::Location* at = nullptr)
			{
				const std::string n = NameOf(local->name);
				const TokenType type = Classes->count(n) ? TokenType::Class
					: Parameters.count(local)            ? TokenType::Parameter
					: function                           ? TokenType::Function
														 : TokenType::Variable;
				Items.push_back({ at ? *at : local->location, n, type, modifiers });
			}
			bool visit(Luau::AstExprFunction* fn) override
			{
				for (Luau::AstLocal* arg : fn->args)
				{
					Parameters.insert(arg);
					Local(arg, Declaration, false);
				}
				return true;
			}
			bool visit(Luau::AstStatLocal* decl) override
			{
				for (size_t i = 0; i < decl->vars.size; i++)
				{
					const bool function = i < decl->values.size && decl->values.data[i]->is<Luau::AstExprFunction>();
					Local(decl->vars.data[i], Declaration, function);
				}
				return true;
			}
			bool visit(Luau::AstStatLocalFunction* fn) override
			{
				Local(fn->name, Declaration, true);
				return true;
			}
			bool visit(Luau::AstStatFor* loop) override
			{
				Local(loop->var, Declaration, false);
				return true;
			}
			bool visit(Luau::AstStatForIn* loop) override
			{
				for (Luau::AstLocal* var : loop->vars)
					Local(var, Declaration, false);
				return true;
			}
			bool visit(Luau::AstExprLocal* e) override
			{
				Local(e->local, 0, IsFunction(e), &e->location);
				return true;
			}
			bool visit(Luau::AstStatFunction* fn) override
			{
				// The name a function statement declares (a script's top-level
				// function is a global of its own).
				if (Luau::AstExprGlobal* g = fn->name->as<Luau::AstExprGlobal>())
					Items.push_back({ g->location, NameOf(g->name), TokenType::Function, Declaration });
				else if (Luau::AstExprIndexName* ix = fn->name->as<Luau::AstExprIndexName>())
					Items.push_back({ ix->indexLocation, NameOf(ix->index), TokenType::Method, Declaration });
				return true;
			}
			bool visit(Luau::AstExprGlobal* e) override
			{
				const std::string n = NameOf(e->name);
				const bool own = FileFunctions.count(n) > 0;
				const TokenType type = Classes->count(n) ? TokenType::Class
					: own || IsFunction(e)              ? TokenType::Function
														: TokenType::Namespace;
				Items.push_back({ e->location, n, type, own ? 0u : (unsigned)DefaultLibrary });
				return true;
			}
			bool visit(Luau::AstExprIndexName* e) override
			{
				Items.push_back({ e->indexLocation, NameOf(e->index), IsFunction(e) ? TokenType::Method : TokenType::Property, 0 });
				return true;
			}
			bool visit(Luau::AstTypeReference* t) override
			{
				const std::string n = NameOf(t->name);
				Items.push_back({ t->nameLocation, n, Classes->count(n) ? TokenType::Class : TokenType::Type, 0 });
				return true;
			}
		} names;
		names.Checked = module.get();
		names.Classes = &classes;
		for (Luau::AstStat* stat : source->root->body)
			if (Luau::AstStatFunction* fn = stat->as<Luau::AstStatFunction>())
				if (Luau::AstExprGlobal* g = fn->name->as<Luau::AstExprGlobal>())
					names.FileFunctions.insert(NameOf(g->name));
		source->root->visit(&names);

		std::map<std::pair<int, int>, SemanticToken> tokens;
		for (const auto& found : names.Items)
		{
			const Range r = Out(name, found.At);
			// Only names the text holds (not the translation's own).
			if (found.Name.empty() || found.Name.rfind("__", 0) == 0 || !IdentifierAt(*text, r.Begin.Line, r.Begin.Col, found.Name))
				continue;
			SemanticToken& token = tokens[{ r.Begin.Line, r.Begin.Col }];
			token.At = r.Begin;
			token.Length = (int)found.Name.size();
			// A declaration's kind wins over what another visit says.
			if ((token.Modifiers & Declaration) && !(found.Modifiers & Declaration))
				continue;
			token.Type = found.Type;
			token.Modifiers = found.Modifiers;
		}
		// The classes' own declarations (their names, members and fields).
		if (translation)
			for (const ClassInfo& c : translation->Classes)
			{
				auto mark = [&](const Range& r, TokenType type) {
					if (r.Begin.Line < 0 || r.End.Col <= r.Begin.Col)
						return;
					tokens[{ r.Begin.Line, r.Begin.Col }] = { r.Begin, r.End.Col - r.Begin.Col, type, Declaration };
				};
				mark(c.At, TokenType::Class);
				for (const ClassMethod& m : c.Methods)
					mark(m.At, TokenType::Method);
				for (const ClassField& f : c.Fields)
					mark(f.At, TokenType::Property);
			}
		for (auto& [key, token] : tokens)
			out.push_back(token);
		return out;
	}

	// ---- Call hierarchy ---------------------------------------------------------

	std::optional<CallItem> LanguageService::CallItemAt(const Location& declaration)
	{
		const std::string module = Name(declaration.Path);
		const std::shared_ptr<const SourceText> text = SourceOf(module);
		if (!text || !Prepare(module))
			return std::nullopt;
		const std::string lineText = LineOf(*text, declaration.At.Begin.Line);
		int end = declaration.At.Begin.Col;
		while (end < (int)lineText.size() && IsIdent(lineText[(size_t)end]))
			end++;
		CallItem item;
		item.Name = lineText.substr((size_t)declaration.At.Begin.Col, (size_t)(end - declaration.At.Begin.Col));
		item.At = { PathOf(module), { declaration.At.Begin, { declaration.At.Begin.Line, end } } };
		item.Detail = fs::u8path(module).filename().u8string();
		if (const TranslateResult* t = m_Analysis.Translation(module))
			for (const ClassInfo& c : t->Classes)
				for (const ClassMethod& m : c.Methods)
					if (m.At.Begin == declaration.At.Begin)
					{
						item.Kind = SymbolKind::Method;
						item.Detail = c.Name;
						return item;
					}
		// A function declared there: `function f`, a local function, or a
		// name a function is given to.
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(module);
		if (!source)
			return std::nullopt;
		for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, In(module, declaration.At.Begin.Line, declaration.At.Begin.Col)))
		{
			if (node->is<Luau::AstStatFunction>() || node->is<Luau::AstStatLocalFunction>())
				return item;
			if (Luau::AstStatLocal* decl = node->as<Luau::AstStatLocal>())
				for (size_t i = 0; i < decl->vars.size && i < decl->values.size; i++)
					if (NameOf(decl->vars.data[i]->name) == item.Name && decl->values.data[i]->is<Luau::AstExprFunction>())
						return item;
		}
		return std::nullopt;
	}

	std::optional<CallItem> LanguageService::PrepareCall(const std::string& path, int line, int col)
	{
		std::optional<Location> found = Definition(path, line, col);
		if (!found)
		{
			const std::vector<Location> uses = References(path, line, col);
			if (uses.empty())
				return std::nullopt;
			found = uses.front();
		}
		return CallItemAt(*found);
	}

	CallItem LanguageService::Enclosing(const std::string& module, const std::string& file, const Luau::Position& at)
	{
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(module);
		const std::shared_ptr<const SourceText> text = SourceOf(module);
		CallItem top;
		top.Name = "(top level)";
		top.Kind = SymbolKind::Function;
		top.Detail = fs::u8path(module).filename().u8string();
		top.At = { file, { { 0, 0 }, { 0, 0 } } };
		if (!source || !text)
			return top;
		const std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, at);
		for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
		{
			Luau::AstExprFunction* fn = (*it)->as<Luau::AstExprFunction>();
			if (!fn)
				continue;
			// The function's name: where its statement (or the local given it) names it.
			std::optional<Luau::Location> nameAt;
			std::string symbol;
			if (it + 1 != ancestry.rend())
			{
				Luau::AstNode* parent = *(it + 1);
				if (Luau::AstStatFunction* stat = parent->as<Luau::AstStatFunction>())
				{
					if (Luau::AstExprGlobal* g = stat->name->as<Luau::AstExprGlobal>())
						nameAt = g->location, symbol = NameOf(g->name);
					else if (Luau::AstExprIndexName* ix = stat->name->as<Luau::AstExprIndexName>())
						nameAt = ix->indexLocation, symbol = NameOf(ix->index);
				}
				else if (Luau::AstStatLocalFunction* local = parent->as<Luau::AstStatLocalFunction>())
					nameAt = local->name->location, symbol = NameOf(local->name->name);
				else if (Luau::AstStatLocal* decl = parent->as<Luau::AstStatLocal>())
					for (size_t i = 0; i < decl->values.size && i < decl->vars.size; i++)
						if (decl->values.data[i] == fn)
							nameAt = decl->vars.data[i]->location, symbol = NameOf(decl->vars.data[i]->name);
			}
			if (nameAt)
				if (std::optional<Location> mapped = Mapped(module, *nameAt, symbol))
					if (std::optional<CallItem> item = CallItemAt({ module, mapped->At }))
						return *item;
			// A function with no name of its own: the next one out.
		}
		return top;
	}

	std::vector<Call> LanguageService::IncomingCalls(const CallItem& item)
	{
		std::vector<Call> out;
		std::map<std::pair<std::string, std::pair<int, int>>, size_t> byCaller;
		for (const Location& use : References(item.At.Path, item.At.At.Begin.Line, item.At.At.Begin.Col))
		{
			if (use.Path == item.At.Path && use.At.Begin == item.At.At.Begin)
				continue; // the declaration
			const std::string module = Name(use.Path);
			const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(module);
			if (!source)
				continue;
			const Luau::Position at = In(module, use.At.Begin.Line, use.At.Begin.Col);
			// A call of it: the name is (the end of) what is called.
			bool called = false;
			for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, at))
				if (Luau::AstExprCall* call = node->as<Luau::AstExprCall>(); call && call->func->location.containsClosed(at))
					called = true;
			if (!called)
				continue;
			const CallItem caller = Enclosing(module, use.Path, at);
			const auto key = std::make_pair(caller.At.Path, std::make_pair(caller.At.At.Begin.Line, caller.At.At.Begin.Col));
			auto found = byCaller.find(key);
			if (found == byCaller.end())
			{
				found = byCaller.emplace(key, out.size()).first;
				out.push_back({ caller, {} });
			}
			out[found->second].Sites.push_back(use.At);
		}
		return out;
	}

	std::vector<Call> LanguageService::OutgoingCalls(const CallItem& item)
	{
		std::vector<Call> out;
		const std::string module = Name(item.At.Path);
		if (!Prepare(module))
			return out;
		const Luau::SourceModule* source = m_Analysis.Frontend().getSourceModule(module);
		const std::shared_ptr<const SourceText> text = SourceOf(module);
		if (!source || !text)
			return out;
		// The function's body: the function whose name is at the item.
		Luau::AstExprFunction* body = nullptr;
		const Luau::Position at = In(module, item.At.At.Begin.Line, item.At.At.Begin.Col);
		for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, at))
		{
			if (Luau::AstStatFunction* stat = node->as<Luau::AstStatFunction>())
				body = stat->func;
			else if (Luau::AstStatLocalFunction* local = node->as<Luau::AstStatLocalFunction>())
				body = local->func;
			else if (Luau::AstStatLocal* decl = node->as<Luau::AstStatLocal>())
				for (size_t i = 0; i < decl->values.size; i++)
					if (Luau::AstExprFunction* fn = decl->values.data[i]->as<Luau::AstExprFunction>())
						body = fn;
		}
		if (!body)
			return out;
		struct Calls : Luau::AstVisitor
		{
			Luau::AstExprFunction* Root = nullptr;
			std::vector<std::pair<Luau::Location, std::string>> Found; // the callee's name
			bool visit(Luau::AstExprFunction* fn) override { return fn == Root; } // not into nested functions
			bool visit(Luau::AstExprCall* call) override
			{
				if (Luau::AstExprIndexName* ix = call->func->as<Luau::AstExprIndexName>())
					Found.push_back({ ix->indexLocation, NameOf(ix->index) });
				else if (Luau::AstExprLocal* local = call->func->as<Luau::AstExprLocal>())
					Found.push_back({ local->location, NameOf(local->local->name) });
				else if (Luau::AstExprGlobal* global = call->func->as<Luau::AstExprGlobal>())
					Found.push_back({ global->location, NameOf(global->name) });
				return true;
			}
		} calls;
		calls.Root = body;
		body->visit(&calls);
		std::map<std::pair<std::string, std::pair<int, int>>, size_t> byCallee;
		for (const auto& [location, symbol] : calls.Found)
		{
			const Range r = Out(module, location);
			if (!IdentifierAt(*text, r.Begin.Line, r.Begin.Col, symbol))
				continue;
			const std::optional<Location> target = Definition(item.At.Path, r.Begin.Line, r.Begin.Col);
			// The workspace's functions (a standard module's are declarations only).
			if (!target || target->Path.empty() || target->Path[0] == '@')
				continue;
			const std::optional<CallItem> callee = CallItemAt(*target);
			if (!callee)
				continue;
			const auto key = std::make_pair(callee->At.Path, std::make_pair(callee->At.At.Begin.Line, callee->At.At.Begin.Col));
			auto found = byCallee.find(key);
			if (found == byCallee.end())
			{
				found = byCallee.emplace(key, out.size()).first;
				out.push_back({ *callee, {} });
			}
			out[found->second].Sites.push_back({ r.Begin, { r.Begin.Line, r.Begin.Col + (int)symbol.size() } });
		}
		return out;
	}

	std::vector<TextEdit> LanguageService::Format(const std::string& path, const std::string& indent)
	{
		std::vector<TextEdit> edits;
		const std::string name = Name(path);
		const std::shared_ptr<const SourceText> text = SourceOf(name);
		if (!text || !IsCluauPath(name))
			return edits;
		FormatOptions options;
		options.Indent = indent;
		const FormatResult result = cluau::Format(text->Text, options);
		if (result.HasErrors() || !result.Changed)
			return edits;
		const int lines = (int)text->Starts.size() - 1;
		edits.push_back({ { { 0, 0 }, { lines + 1, 0 } }, result.Text });
		return edits;
	}

} // namespace cluau::runtime::detail
