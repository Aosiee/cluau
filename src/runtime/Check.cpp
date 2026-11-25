// `cluau check`: Luau's type checker over scripts and the modules they
// require, with the standard modules typed (StdTypes below) and cluau's
// strict rules (cluau_check). Modules resolve as `require` resolves them at
// run time (Resolve.cpp).

#include "cluau/check.h"
#include "cluau/runtime.h"
#include "Resolve.h"
#include "Std.h"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/Frontend.h"
#include "Luau/Linter.h"
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
declare function __cluau_throw(value: unknown): never
declare function __cluau_abstract(class: string, method: string): never
)LUAU";

		// Each standard module's type, as a module the checker reads for
		// `require("@std/<name>")` (the functions are Std.cpp's).
		const std::map<std::string, std::string>& StdTypes()
		{
			static const std::map<std::string, std::string> modules = {
				{ "fs", R"LUAU(
export type File = {
	read: (self: File, count: number?) -> string?,
	readLine: (self: File) -> string?,
	lines: (self: File) -> () -> string?,
	write: (self: File, ...string | number) -> (),
	seek: (self: File, offset: number?, from: ("set" | "current" | "end")?) -> number,
	flush: (self: File) -> (),
	close: (self: File) -> (),
}
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
	glob: (pattern: string) -> { string },
	open: (path: string, mode: ("r" | "w" | "a")?) -> File,
	lines: (path: string) -> () -> string?,
	readBuffer: (path: string) -> buffer,
	writeBuffer: (path: string, bytes: buffer) -> (),
	tempFile: (prefix: string?) -> string,
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
	timeout: number?,
}
export type ExecOptions = { input: string?, cwd: string?, env: { [string]: string }?, timeout: number? }
export type SpawnOptions = { input: boolean?, cwd: string?, env: { [string]: string }?, stderr: ("inherit" | "merge")? }
export type RunResult = { code: number, output: string, error: string, timedOut: boolean }
export type Child = {
	read: (self: Child) -> string?,
	readLine: (self: Child) -> string?,
	lines: (self: Child) -> () -> string?,
	write: (self: Child, ...string | number) -> (),
	closeInput: (self: Child) -> (),
	wait: (self: Child) -> number,
	kill: (self: Child) -> (),
	id: (self: Child) -> number,
}
local m: any = nil
return m :: {
	args: { string },
	script: string,
	executable: string,
	platform: string,
	env: (name: string) -> string?,
	setEnv: (name: string, value: string?) -> (),
	environment: () -> { [string]: string },
	cwd: () -> string,
	chdir: (dir: string) -> (),
	run: (command: string | { string }, options: RunOptions?) -> RunResult,
	exec: (command: string | { string }, options: ExecOptions?) -> (number, boolean),
	spawn: (command: string | { string }, options: SpawnOptions?) -> Child,
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
	decode: (text: string, options: { null: unknown }?) -> unknown,
	null: unknown,
	object: (<T>(value: T) -> T) & (() -> { [string]: unknown }),
	array: (<T>(value: T) -> T) & (() -> { unknown }),
}
)LUAU" },
				{ "http", R"LUAU(
export type Options = {
	headers: { [string]: string }?,
	body: string?,
	json: unknown?,
	timeout: number?,
	followRedirects: boolean?,
	caFile: string?,
}
export type RequestOptions = {
	url: string,
	method: string?,
	headers: { [string]: string }?,
	body: string?,
	json: unknown?,
	timeout: number?,
	followRedirects: boolean?,
	caFile: string?,
}
export type Response = {
	status: number,
	ok: boolean,
	headers: { [string]: string },
	body: string,
	url: string,
}
local m: any = nil
return m :: {
	request: (options: RequestOptions) -> Response,
	get: (url: string, options: Options?) -> Response,
	post: (url: string, body: string?, options: Options?) -> Response,
	download: (url: string, path: string, options: Options?) -> Response,
}
)LUAU" },
				{ "time", R"LUAU(
export type Options = { utc: boolean? }
export type Parts = {
	year: number,
	month: number,
	day: number,
	hour: number,
	minute: number,
	second: number,
	millisecond: number,
	weekday: number,
	yearday: number,
	offset: number,
}
export type PartsGiven = {
	year: number,
	month: number?,
	day: number?,
	hour: number?,
	minute: number?,
	second: number?,
	millisecond: number?,
}
local m: any = nil
return m :: {
	now: () -> number,
	monotonic: () -> number,
	iso: (t: number?, options: Options?) -> string,
	parse: (text: string) -> number,
	format: (t: number, pattern: string, options: Options?) -> string,
	parts: (t: number?, options: Options?) -> Parts,
	fromParts: (parts: PartsGiven, options: Options?) -> number,
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

		// The lint warnings `--lint` reports: Luau's, without those the
		// translation's layout would raise (several statements on a class's
		// line) or the type checker already reports.
		Luau::LintOptions LintOptions()
		{
			Luau::LintOptions options;
			options.warningMask = ~0ull;
			for (Luau::LintWarning::Code code : { Luau::LintWarning::Code_SameLineStatement, Luau::LintWarning::Code_MultiLineStatement,
					 Luau::LintWarning::Code_UnknownGlobal, Luau::LintWarning::Code_ImplicitReturn, Luau::LintWarning::Code_CommentDirective })
				options.warningMask &= ~(1ull << code);
			return options;
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
					// A module written in cluau is typed from its own source.
					if (auto source = detail::StdSources().find(name.substr(5)); source != detail::StdSources().end())
					{
						TranslateOptions options;
						options.Script = true;
						options.Profile = &ScriptProfile();
						// Kept: errors name its classes (Parsed, not its structure).
						const TranslateResult& t = Translations[name] = Translate(source->second, options);
						return Luau::SourceCode{ t.Luau, Luau::SourceCode::Module };
					}
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

			std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node, const Luau::TypeCheckLimits&) override
			{
				auto* expr = node->as<Luau::AstExprConstantString>();
				if (!expr)
					return std::nullopt;
				const std::string request(expr->value.data, expr->value.size);
				const int line = (int)expr->location.begin.line;
				if (StartsWith(request, "@std/"))
				{
					if (!StdTypes().count(request.substr(5)) && !detail::StdSources().count(request.substr(5)))
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
			const Luau::Config& getConfig(const Luau::ModuleName& name, const Luau::TypeCheckLimits&) const override
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
		// Luau's original type solver: the new one (Luau 0.748) does not yet
		// find a subclass's inherited methods through its intersection type,
		// and misreads pcall's arguments and optional option tables.
		Luau::Frontend frontend(Luau::SolverMode::Old, &files, &modes, frontendOptions);
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

		// A subclass's type prints as its whole structure (Luau names an
		// intersection by its parts): errors say the class's name instead.
		std::vector<std::pair<std::string, std::string>> classNames; // printed type, class name
		for (const auto& [name, translation] : files.Translations)
			if (Luau::ModulePtr m = frontend.moduleResolver.getModule(name))
				for (const ClassInfo& c : translation.Classes)
					if (auto it = m->exportedTypeBindings.find(c.Name); it != m->exportedTypeBindings.end())
					{
						// Its structure, as a copy of the type in another module prints.
						Luau::ToStringOptions structural;
						structural.ignoreSyntheticName = true;
						const std::string printed = Luau::toString(it->second.type, structural);
						if (printed != c.Name)
							classNames.emplace_back(printed, c.Name);
					}
		std::sort(classNames.begin(), classNames.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
		// A quoted type in an error is the class's when it reads as the class's
		// type does (whole, or cut short by Luau: `... *TRUNCATED*` after a
		// prefix of it), `{| |}` and `{ }` alike.
		auto plain = [](std::string text) {
			for (const char* marker : { "{|", "|}" })
				for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at))
					text.erase(marker[0] == '{' ? at + 1 : at, 1);
			return text;
		};
		for (auto& [printed, name] : classNames)
			printed = plain(printed);
		auto nameClasses = [&](std::string message) {
			constexpr std::string_view kCut = "... *TRUNCATED*";
			for (size_t open = message.find('\''); open != std::string::npos; open = message.find('\'', open + 1))
			{
				const size_t close = message.find('\'', open + 1);
				if (close == std::string::npos)
					break;
				std::string quoted = plain(message.substr(open + 1, close - open - 1));
				// An optional class prints as `t1? where t1 = ...`.
				std::string suffix;
				if (const size_t mark = quoted.find("? where "); mark != std::string::npos && mark > 0 &&
					quoted.find_first_of(" {(<", 0) == mark + 1)
				{
					quoted.erase(mark, 1);
					suffix = "?";
				}
				for (const auto& [printed, name] : classNames)
				{
					const bool cut = quoted.size() > kCut.size() && quoted.compare(quoted.size() - kCut.size(), kCut.size(), kCut) == 0;
					const size_t length = cut ? quoted.size() - kCut.size() : quoted.size();
					if (cut ? printed.compare(0, length, quoted, 0, length) == 0 : printed == quoted)
					{
						message.replace(open + 1, close - open - 1, name + suffix);
						break;
					}
				}
				const size_t next = message.find('\'', open + 1);
				if (next == std::string::npos)
					break;
				open = next; // past the closing quote
			}
			// "Not all intersection parts are compatible", with one part of
			// a subclass's type: the reason after it says what is wrong.
			constexpr std::string_view kParts = "caused by:\n  Not all intersection parts are compatible.\n";
			if (const size_t parts = message.find(kParts); parts != std::string::npos)
				if (const size_t next = message.find("caused by:\n", parts + kParts.size()); next != std::string::npos)
					message.erase(parts, next - parts);
			return message;
		};
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
				for (const ClassInfo& c : translation->Classes)
					if (c.UncheckedConstructor)
						out.push_back({ display, c.At, "UncheckedConstructor",
							c.Name + "(...)'s arguments are not checked: its constructor comes from " + c.Base +
								", another module's class (declare Init in " + c.Name + " to check them)",
							true });
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
				f.Message = nameClasses(Luau::toString(e, Luau::TypeErrorToStringOptions{ &files }));
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
			const Luau::SourceModule* source = frontend.getSourceModule(name);
			Luau::ModulePtr module = frontend.moduleResolver.getModule(name);
			if (options.Lint && source && source->root && module)
				for (const Luau::LintWarning& w :
					Luau::lint(source->root, *source->names, frontend.globals.globalScope, module.get(), source->hotcomments, LintOptions()))
				{
					Position begin{ (int)w.location.begin.line, (int)w.location.begin.column };
					if (translation)
					{
						// Not in what the translation generated (a class's header).
						bool generated = false;
						for (const Range& g : translation->Generated)
							generated = generated || (begin.Line == g.Begin.Line && begin.Col >= g.Begin.Col && begin.Col < g.End.Col);
						if (generated)
							continue;
						begin = translation->Map.FromLuau(begin);
					}
					out.push_back({ display, { begin, begin }, std::string("lint:") + Luau::LintWarning::getName(w.code), w.text, true });
				}
			if (translation && !options.Loose)
			{
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
