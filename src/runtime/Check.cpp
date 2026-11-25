// `cluau check`: Luau's type checker over scripts and the modules they
// require, with the standard modules typed (StdTypes below) and cluau's
// strict rules (cluau_check). Modules resolve as `require` resolves them at
// run time (Resolve.cpp).

#include "cluau/check.h"
#include "cluau/runtime.h"
#include "Resolve.h"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/Frontend.h"
#include "Luau/Error.h"
#include "Luau/ToString.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace cluau::runtime {

	namespace {

		// The runtime's globals beyond Luau's (Runtime.cpp, Std.cpp).
		constexpr const char* kGlobals = R"LUAU(
declare function repr(value: unknown): string
declare function input(prompt: string?): string?
declare __cluau_meta: unknown
)LUAU";

		// Each standard module's type, as a module the checker reads for
		// `require("@std/<name>")` (the functions are Std.cpp's).
		const std::map<std::string, std::string>& StdTypes()
		{
			static const std::map<std::string, std::string> modules = {
				{ "fs", R"LUAU(
local m: any = nil
return m :: {
	read: (path: string) -> string,
	write: (path: string, text: string) -> (),
	append: (path: string, text: string) -> (),
	exists: (path: string) -> boolean,
	isFile: (path: string) -> boolean,
	isDir: (path: string) -> boolean,
	list: (dir: string) -> { string },
	walk: (dir: string) -> { string },
	mkdir: (path: string) -> (),
	remove: (path: string, all: boolean?) -> (),
	rename: (from: string, to: string) -> (),
	copy: (from: string, to: string) -> (),
	size: (path: string) -> number,
	modified: (path: string) -> number,
	tempDir: () -> string,
}
)LUAU" },
				{ "path", R"LUAU(
local m: any = nil
return m :: {
	join: (...string) -> string,
	dirname: (path: string) -> string,
	basename: (path: string) -> string,
	stem: (path: string) -> string,
	extension: (path: string) -> string,
	normalize: (path: string) -> string,
	absolute: (path: string) -> string,
	relative: (path: string, base: string?) -> string,
	isAbsolute: (path: string) -> boolean,
}
)LUAU" },
				{ "process", R"LUAU(
export type RunOptions = {
	input: string?,
	cwd: string?,
	env: { [string]: string }?,
	stderr: ("inherit" | "capture" | "merge")?,
}
export type ExecOptions = { input: string?, cwd: string?, env: { [string]: string }? }
export type RunResult = { code: number, output: string, error: string }
local m: any = nil
return m :: {
	args: { string },
	script: string,
	executable: string,
	platform: string,
	env: (name: string) -> string?,
	setEnv: (name: string, value: string?) -> (),
	cwd: () -> string,
	chdir: (dir: string) -> (),
	run: (command: string | { string }, options: RunOptions?) -> RunResult,
	exec: (command: string | { string }, options: ExecOptions?) -> number,
	sleep: (seconds: number) -> (),
	exit: (code: number?) -> never,
}
)LUAU" },
				{ "io", R"LUAU(
local m: any = nil
return m :: {
	write: (...unknown) -> (),
	writeError: (...unknown) -> (),
	readLine: () -> string?,
	readAll: () -> string,
	flush: () -> (),
}
)LUAU" },
				{ "json", R"LUAU(
local m: any = nil
return m :: {
	encode: (value: unknown, pretty: (boolean | string)?) -> string,
	decode: (text: string) -> unknown,
}
)LUAU" },
			};
			return modules;
		}

		bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

		std::optional<std::string> ReadFile(const fs::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return std::nullopt;
			std::stringstream buffer;
			buffer << in.rdbuf();
			return buffer.str();
		}

		bool IsCluau(const std::string& name) { return !StartsWith(name, "@") && !detail::IsLuauFile(fs::u8path(name)); }

		struct Files : Luau::FileResolver
		{
			fs::path StartDir;
			std::vector<fs::path> SearchDirs;
			std::map<std::string, fs::path> MainDirs;           // a checked script's module name -> its directory
			std::map<std::string, std::string> RootOf;          // module name -> the checked script that reached it
			std::map<std::string, TranslateResult> Translations; // .cluau modules read
			std::vector<std::string> UserModules;               // files read, in order
			// Requires that name no module: (module, line) -> what `require` says.
			std::map<std::pair<std::string, int>, std::string> NotFound;

			std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override
			{
				if (StartsWith(name, "@std/"))
				{
					auto it = StdTypes().find(name.substr(5));
					if (it == StdTypes().end())
						return std::nullopt;
					return Luau::SourceCode{ it->second, Luau::SourceCode::Module };
				}
				std::optional<std::string> text = ReadFile(fs::u8path(name));
				if (!text)
					return std::nullopt;
				if (std::find(UserModules.begin(), UserModules.end(), name) == UserModules.end())
					UserModules.push_back(name);
				if (!IsCluau(name))
					return Luau::SourceCode{ *text, Luau::SourceCode::Module };
				TranslateOptions options;
				options.Script = true;
				options.Profile = &ScriptProfile();
				TranslateResult& t = Translations[name] = Translate(*text, options);
				return Luau::SourceCode{ t.Luau, Luau::SourceCode::Module };
			}

			std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node) override
			{
				auto* expr = node->as<Luau::AstExprConstantString>();
				if (!expr)
					return std::nullopt;
				const std::string request(expr->value.data, expr->value.size);
				const int line = (int)expr->location.begin.line;
				if (StartsWith(request, "@std/"))
				{
					if (!StdTypes().count(request.substr(5)))
					{
						if (context)
							NotFound[{ context->name, line }] = "no standard module '" + request + "'";
						return std::nullopt;
					}
					return Luau::ModuleInfo{ request };
				}
				if (StartsWith(request, "@") || !context)
				{
					if (context)
						NotFound[{ context->name, line }] = "unknown alias in '" + request + "' (the standard modules are @std/<name>)";
					return std::nullopt;
				}
				const std::string& from = context->name;
				const std::string root = RootOf.count(from) ? RootOf[from] : from;
				const fs::path mainDir = MainDirs.count(root) ? MainDirs[root] : StartDir;
				std::vector<fs::path> looked;
				const fs::path found = detail::ResolveRequire(request, fs::u8path(from).parent_path(), mainDir, SearchDirs, &looked);
				if (found.empty())
				{
					std::string tried;
					for (const fs::path& base : looked)
						tried += (tried.empty() ? "" : ", ") + Display(detail::PathText(base)) + "[.cluau|.luau]";
					NotFound[{ from, line }] = "module '" + request + "' not found (looked for " + tried + ")";
					return std::nullopt;
				}
				const std::string name = detail::ModuleKey(found);
				if (!RootOf.count(name))
					RootOf[name] = root;
				return Luau::ModuleInfo{ name };
			}

			std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override { return Display(name); }

			std::string Display(const std::string& name) const
			{
				if (StartsWith(name, "@"))
					return name;
				const fs::path rel = fs::u8path(name).lexically_relative(StartDir);
				if (!rel.empty() && *rel.begin() != "..")
					return detail::PathText(rel);
				return name;
			}
		};

		// .cluau modules and the standard modules in strict mode; .luau
		// modules in their own (Luau's default, nonstrict).
		struct Modes : Luau::ConfigResolver
		{
			Luau::Config Nonstrict;
			Luau::Config Strict;
			Modes()
			{
				Nonstrict.mode = Luau::Mode::Nonstrict;
				Strict.mode = Luau::Mode::Strict;
			}
			const Luau::Config& getConfig(const Luau::ModuleName& name) const override
			{
				return IsCluau(name) || StartsWith(name, "@std/") ? Strict : Nonstrict;
			}
		};

	} // namespace

	std::vector<CheckFinding> Check(const std::vector<std::string>& paths, const CheckOptions& options)
	{
		std::vector<CheckFinding> out;
		Files files;
		std::error_code ec;
		files.StartDir = fs::current_path(ec);
		for (const std::string& dir : options.SearchPath)
			files.SearchDirs.push_back(files.StartDir / fs::u8path(dir));
		Modes modes;
		Luau::FrontendOptions frontendOptions;
		frontendOptions.retainFullTypeGraphs = true; // the strict rules read the inferred types
		Luau::Frontend frontend(&files, &modes, frontendOptions);
		Luau::registerBuiltinGlobals(frontend, frontend.globals);
		Luau::LoadDefinitionFileResult loaded =
			frontend.loadDefinitionFile(frontend.globals, frontend.globals.globalScope, kGlobals, "@cluau", false, false);
		if (!loaded.success)
			out.push_back({ "<runtime globals>", {}, "Internal", "the runtime's global declarations do not load", false });
		Luau::freeze(frontend.globals.globalTypes);

		std::vector<std::string> roots;
		for (const std::string& path : paths)
		{
			const fs::path file = files.StartDir / fs::u8path(path);
			if (!fs::is_regular_file(file, ec))
			{
				out.push_back({ path, {}, "Missing", "cannot read the file", false });
				continue;
			}
			const std::string name = detail::ModuleKey(file);
			files.MainDirs[name] = fs::u8path(name).parent_path();
			files.RootOf[name] = name;
			roots.push_back(name);
		}

		std::vector<Luau::TypeError> errors;
		for (const std::string& root : roots)
		{
			Luau::CheckResult result = frontend.check(root);
			errors.insert(errors.end(), result.errors.begin(), result.errors.end());
		}

		std::set<std::string> reported;
		for (const std::string& name : files.UserModules)
		{
			if (!reported.insert(name).second)
				continue;
			const std::string display = files.Display(name);
			auto tr = files.Translations.find(name);
			const TranslateResult* translation = tr == files.Translations.end() ? nullptr : &tr->second;
			if (translation)
			{
				for (const Diagnostic& d : translation->Diagnostics)
					out.push_back({ display, d.At, "cluau", d.Message, d.Warning });
				if (translation->HasErrors())
					continue; // the translation's Luau is not the script: its errors would mislead
			}
			std::set<std::string> seen; // Luau reports a module's errors once per checked root
			std::set<int> unresolvedLines;
			for (const Luau::TypeError& e : errors)
			{
				if (e.moduleName != name)
					continue;
				CheckFinding f;
				f.File = display;
				f.Code = Luau::get<Luau::SyntaxError>(e) ? "SyntaxError" : "TypeError";
				f.Message = Luau::toString(e, Luau::TypeErrorToStringOptions{ &files });
				if (Luau::get<Luau::UnknownRequire>(e))
				{
					auto why = files.NotFound.find({ name, (int)e.location.begin.line });
					if (why != files.NotFound.end())
						f.Message = why->second;
					unresolvedLines.insert((int)e.location.begin.line);
				}
				Position begin{ (int)e.location.begin.line, (int)e.location.begin.column };
				Position end{ (int)e.location.end.line, (int)e.location.end.column };
				if (translation)
				{
					begin = translation->Map.FromLuau(begin);
					end = translation->Map.FromLuau(end);
				}
				f.At = { begin, end };
				const std::string key = std::to_string(begin.Line) + ":" + std::to_string(begin.Col) + ":" + f.Message;
				if (seen.insert(key).second)
					out.push_back(std::move(f));
			}
			if (translation)
			{
				const Luau::SourceModule* source = frontend.getSourceModule(name);
				Luau::ModulePtr module = frontend.moduleResolver.getModule(name);
				if (source && module)
					for (const check::Finding& c : check::Run(*source, *module, *translation))
					{
						// An unresolved require's value is any: the not-found error says it.
						if (c.Code == "NoAny" && unresolvedLines.count(translation->Map.ToLuau(c.At.Begin).Line))
							continue;
						out.push_back({ display, c.At, c.Code, c.Message, c.Warning });
					}
			}
		}
		std::stable_sort(out.begin(), out.end(), [](const CheckFinding& a, const CheckFinding& b) {
			return a.File != b.File ? a.File < b.File : a.At.Begin < b.At.Begin;
		});
		return out;
	}

} // namespace cluau::runtime
