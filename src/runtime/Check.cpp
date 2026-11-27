// `cluau check`: Luau's type checker over scripts and the modules they
// require, with the standard modules typed (StdTypes below) and cluau's
// strict rules (cluau_check). Modules resolve as `require` resolves them at
// run time (Resolve.cpp).

#include "cluau/check.h"
#include "cluau/runtime.h"
#include "Resolve.h"
#include "Std.h"
#include "Analysis.h"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/Frontend.h"
#include "Luau/Linter.h"
#include "Luau/Parser.h"
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
			static const std::map<std::string, std::string> modules = [] {
				std::map<std::string, std::string> all = {
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
	realpath: (path: string) -> string,
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
	encode: (value: unknown, pretty: (boolean | number | string)?) -> string,
	decode: (text: string, options: { null: unknown }?) -> unknown,
	null: unknown,
	object: (<T>(value: T) -> T) & (() -> { [string]: unknown }),
	array: (<T>(value: T) -> T) & (() -> { unknown }),
}
)LUAU" },
				{ "_jobs", R"LUAU(
local m: any = nil
return m :: {
	marker: unknown,
	done: (id: number) -> boolean,
	pending: () -> number,
	completed: () -> number,
	wait: (seconds: number?, since: number?) -> (),
	forget: (id: number) -> (),
	register: (tasks: { [thread]: unknown }, runUntilJob: (id: number) -> ()) -> (),
}
)LUAU" },
				{ "net", R"LUAU(
export type Socket = {
	tls: boolean,
	localHost: string,
	localPort: number,
	remoteHost: string,
	remotePort: number,
	read: (self: Socket, count: number?) -> string?,
	readLine: (self: Socket, limit: number?) -> string?,
	readAll: (self: Socket) -> string,
	lines: (self: Socket) -> () -> string?,
	write: (self: Socket, ...string) -> (),
	setTimeout: (self: Socket, seconds: number?) -> (),
	setDeadline: (self: Socket, seconds: number?) -> (),
	close: (self: Socket) -> (),
}
export type Server = {
	tls: boolean,
	host: string,
	port: number,
	accept: (self: Server, options: { timeout: number? }?) -> Socket,
	close: (self: Server) -> (),
}
local m: any = nil
return m :: {
	connect: (host: string, port: number, options: { timeout: number?, tls: boolean?, verify: boolean?, caFile: string?, serverName: string? }?) -> Socket,
	listen: (host: string, port: number, options: { backlog: number?, tls: { certFile: string, keyFile: string }? }?) -> Server,
}
)LUAU" },
				{ "sqlite", R"LUAU(
export type Value = nil | boolean | number | string | buffer
export type Changes = { changes: number, lastId: number }
export type Statement = {
	query: <T>(self: Statement, ...unknown) -> { T },
	first: <T>(self: Statement, ...unknown) -> T?,
	rows: <T>(self: Statement, ...unknown) -> () -> T?,
	run: (self: Statement, ...unknown) -> Changes,
	columns: (self: Statement) -> { string },
	close: (self: Statement) -> (),
}
export type Database = {
	path: string,
	exec: (self: Database, sql: string) -> (),
	query: <T>(self: Database, sql: string, ...unknown) -> { T },
	first: <T>(self: Database, sql: string, ...unknown) -> T?,
	rows: <T>(self: Database, sql: string, ...unknown) -> () -> T?,
	run: (self: Database, sql: string, ...unknown) -> Changes,
	prepare: (self: Database, sql: string) -> Statement,
	transaction: <A..., R...>(self: Database, fn: (A...) -> R..., A...) -> R...,
	close: (self: Database) -> (),
}
local m: any = nil
return m :: {
	open: (path: string, options: { readonly: boolean?, create: boolean? }?) -> Database,
	version: string,
}
)LUAU" },
				{ "crypto", R"LUAU(
export type Algorithm = "md5" | "sha1" | "sha224" | "sha256" | "sha384" | "sha512"
export type Hasher = {
	update: (self: Hasher, data: string | buffer) -> Hasher,
	digest: (self: Hasher, options: { raw: boolean? }?) -> string,
}
local m: any = nil
return m :: {
	hash: (algorithm: Algorithm, data: string | buffer, options: { raw: boolean? }?) -> string,
	hmac: (algorithm: Algorithm, key: string | buffer, data: string | buffer, options: { raw: boolean? }?) -> string,
	hasher: (algorithm: Algorithm) -> Hasher,
	randomBytes: (count: number) -> string,
	uuid: () -> string,
	equal: (a: string | buffer, b: string | buffer) -> boolean,
}
)LUAU" },
				{ "encoding", R"LUAU(
local m: any = nil
return m :: {
	toBase64: (data: string | buffer, options: { url: boolean?, padding: boolean? }?) -> string,
	fromBase64: (text: string | buffer) -> string,
	toHex: (data: string | buffer) -> string,
	fromHex: (text: string | buffer) -> string,
}
)LUAU" },
				{ "random", R"LUAU(
export type Random = {
	seed: (self: Random, seed: number?) -> (),
	random: (self: Random) -> number,
	integer: (self: Random, min: number, max: number) -> number,
	uniform: (self: Random, a: number, b: number) -> number,
	normal: (self: Random, mean: number?, deviation: number?) -> number,
	choice: <T>(self: Random, list: { T }) -> T,
	shuffle: <T>(self: Random, list: { T }) -> (),
	sample: <T>(self: Random, list: { T }, count: number) -> { T },
	weighted: <T>(self: Random, list: { T }, weights: { number }) -> T,
}
local m: any = nil
return m :: {
	new: (seed: number?) -> Random,
	seed: (seed: number?) -> (),
	random: () -> number,
	integer: (min: number, max: number) -> number,
	uniform: (a: number, b: number) -> number,
	normal: (mean: number?, deviation: number?) -> number,
	choice: <T>(list: { T }) -> T,
	shuffle: <T>(list: { T }) -> (),
	sample: <T>(list: { T }, count: number) -> { T },
	weighted: <T>(list: { T }, weights: { number }) -> T,
}
)LUAU" },
				{ "worker", R"LUAU(
export type Worker = {
	done: boolean,
	send: (self: Worker, value: unknown) -> (),
	receive: (self: Worker, seconds: number?) -> unknown,
	join: (self: Worker) -> ...unknown,
	terminate: (self: Worker) -> (),
}
local m: any = nil
return m :: {
	start: (path: string, ...unknown) -> Worker,
	isWorker: boolean,
	args: { unknown },
	send: (value: unknown) -> (),
	receive: (seconds: number?) -> unknown,
}
)LUAU" },
				{ "compress", R"LUAU(
local m: any = nil
return m :: {
	gzip: (data: string | buffer, options: { level: number? }?) -> string,
	gunzip: (data: string | buffer) -> string,
	deflate: (data: string | buffer, options: { level: number?, raw: boolean? }?) -> string,
	inflate: (data: string | buffer, options: { raw: boolean? }?) -> string,
}
)LUAU" },
				{ "zip", R"LUAU(
export type Entry = { name: string, size: number, compressedSize: number, directory: boolean, modified: number }
export type Archive = {
	entries: (self: Archive) -> { Entry },
	read: (self: Archive, name: string) -> string,
	extract: (self: Archive, directory: string) -> number,
	close: (self: Archive) -> (),
}
export type Writer = {
	add: (self: Writer, name: string, data: string | buffer, options: { level: number? }?) -> Writer,
	addFile: (self: Writer, name: string, path: string, options: { level: number? }?) -> Writer,
	addDirectory: (self: Writer, name: string) -> Writer,
	finish: (self: Writer) -> string?,
}
local m: any = nil
return m :: {
	open: (path: string) -> Archive,
	fromString: (data: string | buffer) -> Archive,
	create: (path: string?) -> Writer,
}
)LUAU" },
				{ "term", R"LUAU(
export type Stream = "stdout" | "stderr" | "stdin"
type Styler = (text: string) -> string
local m: any = nil
return m :: {
	isTerminal: (stream: Stream?) -> boolean,
	colors: (stream: Stream?) -> boolean,
	size: () -> { columns: number, rows: number }?,
	style: (text: string, ...string) -> string,
	strip: (text: string) -> string,
	bold: Styler, dim: Styler, italic: Styler, underline: Styler, inverse: Styler, strike: Styler,
	black: Styler, red: Styler, green: Styler, yellow: Styler, blue: Styler, magenta: Styler, cyan: Styler, white: Styler,
	gray: Styler, grey: Styler, brightRed: Styler, brightGreen: Styler, brightYellow: Styler, brightBlue: Styler,
	brightMagenta: Styler, brightCyan: Styler, brightWhite: Styler,
	bgBlack: Styler, bgRed: Styler, bgGreen: Styler, bgYellow: Styler, bgBlue: Styler, bgMagenta: Styler, bgCyan: Styler,
	bgWhite: Styler, bgGray: Styler,
}
)LUAU" },
				{ "regex", R"LUAU(
export type Match = {
	text: string,
	start: number,
	finish: number,
	groups: { [number]: string },
	named: { [string]: string },
}
export type Regex = {
	pattern: string,
	test: (self: Regex, text: string, start: number?) -> boolean,
	match: (self: Regex, text: string, start: number?) -> Match?,
	matches: (self: Regex, text: string) -> () -> Match?,
	replace: (self: Regex, text: string, replacement: string | (match: Match) -> string, count: number?) -> (string, number),
	split: (self: Regex, text: string, limit: number?) -> { string },
}
local m: any = nil
return m :: {
	compile: (pattern: string, flags: string?) -> Regex,
	escape: (text: string) -> string,
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
				// A module this build leaves out is not typed: `cluau check`
				// reports requiring it, as running the script would.
				for (const char* name : {
#ifndef CLUAU_HAS_CRYPTO
						 "crypto",
#endif
#ifndef CLUAU_HAS_COMPRESS
						 "compress", "zip",
#endif
#ifndef CLUAU_HAS_HTTP
						 "http",
#endif
#ifndef CLUAU_HAS_REGEX
						 "regex",
#endif
#ifndef CLUAU_HAS_SQLITE
						 "sqlite",
#endif
						 "" })
					all.erase(name);
				return all;
			}();
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
			std::string text = buffer.str();
			// A UTF-8 byte order mark is not code (the runtime skips it too).
			if (text.rfind("\xEF\xBB\xBF", 0) == 0)
				text.erase(0, 3);
			return text;
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

		// A chunk with no `return` of its own (outside its functions):
		// `require` gives `true` for it at run time.
		bool ReturnsNothing(const std::string& luau)
		{
			Luau::Allocator allocator;
			Luau::AstNameTable names(allocator);
			const Luau::ParseResult parsed = Luau::Parser::parse(luau.data(), luau.size(), names, allocator);
			if (!parsed.root || !parsed.errors.empty())
				return false;
			struct Returns : Luau::AstVisitor
			{
				bool Found = false;
				bool visit(Luau::AstExprFunction*) override { return false; }
				bool visit(Luau::AstStatReturn*) override
				{
					Found = true;
					return false;
				}
			} returns;
			parsed.root->visit(&returns);
			return !returns.Found;
		}

		// A file's chunk as the checker reads it: one that returns nothing
		// returns `true`, as `require` gives it (the line added after the
		// last keeps every position).
		Luau::SourceCode ModuleSource(std::string luau)
		{
			if (ReturnsNothing(luau))
				luau += "\nreturn true\n";
			return Luau::SourceCode{ std::move(luau), Luau::SourceCode::Module };
		}

		struct Files : Luau::FileResolver
		{
			fs::path StartDir;
			std::vector<fs::path> SearchDirs;
			const std::map<std::string, std::string>* HostTypes = nullptr;   // CheckOptions::Modules
			const std::map<std::string, std::string>* HostSources = nullptr; // CheckOptions::ModuleSources
			std::map<std::string, fs::path> MainDirs;           // a checked script's module name -> its directory
			std::map<std::string, std::string> RootOf;          // module name -> the checked script that reached it
			std::map<std::string, TranslateResult> Translations; // .cluau modules read
			std::vector<std::string> UserModules;               // files read, in order
			// Requires that name no module: (module, line) -> what `require` says.
			std::map<std::pair<std::string, int>, std::string> NotFound;
			std::map<std::string, std::string> Buffers; // an editor's unsaved text, by module name
			std::map<std::string, std::string> Texts;   // each file's text as read (before translation)

			std::optional<std::string> TextOf(const std::string& name) const
			{
				if (auto buffer = Buffers.find(name); buffer != Buffers.end())
					return buffer->second;
				return ReadFile(fs::u8path(name));
			}

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
				// The host's modules: their errors are reported as the files' are.
				if (auto it = HostSources->find(name); it != HostSources->end())
				{
					if (std::find(UserModules.begin(), UserModules.end(), name) == UserModules.end())
						UserModules.push_back(name);
					TranslateOptions options;
					options.Script = true;
					options.Profile = &ScriptProfile();
					const TranslateResult& t = Translations[name] = Translate(it->second, options);
					return ModuleSource(t.Luau);
				}
				if (auto it = HostTypes->find(name); it != HostTypes->end())
				{
					if (std::find(UserModules.begin(), UserModules.end(), name) == UserModules.end())
						UserModules.push_back(name);
					return Luau::SourceCode{ it->second, Luau::SourceCode::Module };
				}
				std::optional<std::string> text = TextOf(name);
				if (!text)
					return std::nullopt;
				Texts[name] = *text;
				for (auto it = NotFound.begin(); it != NotFound.end();)
					it = it->first.first == name ? NotFound.erase(it) : std::next(it);
				if (std::find(UserModules.begin(), UserModules.end(), name) == UserModules.end())
					UserModules.push_back(name);
				// A NUL byte ends the text for Luau's lexer; the runtime
				// refuses such a file, and so does the check.
				if (const size_t nul = text->find('\0'); nul != std::string::npos)
				{
					TranslateResult& t = Translations[name] = TranslateResult{};
					const int line = (int)std::count(text->begin(), text->begin() + (std::ptrdiff_t)nul, '\n');
					const size_t lineStart = text->rfind('\n', nul);
					const int col = (int)(nul - (lineStart == std::string::npos ? 0 : lineStart + 1));
					t.Diagnostics.push_back({ { { line, col }, { line, col + 1 } }, "the source contains a NUL byte", false });
					return Luau::SourceCode{ "return true", Luau::SourceCode::Module };
				}
				if (!IsCluau(name))
					return ModuleSource(*text);
				TranslateOptions options;
				options.Script = true;
				options.Profile = &ScriptProfile();
				TranslateResult& t = Translations[name] = Translate(*text, options);
				return ModuleSource(t.Luau);
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
				if (HostTypes->count(request) || HostSources->count(request))
					return Luau::ModuleInfo{ request };
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
			const Files* Modules = nullptr;
			Luau::Config Nonstrict;
			Luau::Config Strict;
			Modes()
			{
				Nonstrict.mode = Luau::Mode::Nonstrict;
				Strict.mode = Luau::Mode::Strict;
			}
			const Luau::Config& getConfig(const Luau::ModuleName& name, const Luau::TypeCheckLimits&) const override
			{
				const bool strict =
					IsCluau(name) || StartsWith(name, "@std/") || Modules->HostTypes->count(name) || Modules->HostSources->count(name);
				return strict ? Strict : Nonstrict;
			}
		};

	} // namespace

	namespace {

		// `{|` and `|}` read as `{` and `}`.
		std::string Plain(std::string text)
		{
			for (const char* marker : { "{|", "|}" })
				for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at))
					text.erase(marker[0] == '{' ? at + 1 : at, 1);
			return text;
		}

		// A subclass's type prints as its whole structure (Luau names an
		// intersection by its parts): each class's type as it prints (plain,
		// longest first), with the class's name, so messages can say the name.
		std::vector<std::pair<std::string, std::string>> ClassTypeNames(Luau::Frontend& frontend, const Files& files)
		{
			std::vector<std::pair<std::string, std::string>> classNames; // printed type, class name
			for (const auto& [moduleName, translation] : files.Translations)
			{
				// A module marked dirty since its check (it changed, or a
				// module it requires did) can hold types of a module checked
				// again since, which are freed: only modules checked as they
				// are now are read.
				if (frontend.isDirty(moduleName))
					continue;
				if (Luau::ModulePtr m = frontend.moduleResolver.getModule(moduleName))
					for (const ClassInfo& c : translation.Classes)
						if (auto it = m->exportedTypeBindings.find(c.Name); it != m->exportedTypeBindings.end())
						{
							// Its structure, as a copy of the type in another module prints.
							Luau::ToStringOptions structural;
							structural.ignoreSyntheticName = true;
							const std::string printed = Luau::toString(it->second.type, structural);
							if (printed != c.Name)
								classNames.emplace_back(Plain(printed), c.Name);
						}
			}
			std::sort(classNames.begin(), classNames.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
			return classNames;
		}

	} // namespace

	struct detail::Analysis::Impl
	{
		CheckOptions Options;
		struct Files Files;
		struct Modes Modes;
		std::unique_ptr<Luau::Frontend> Frontend;
		bool GlobalsLoaded = true;
	};

	detail::Analysis::Analysis(const CheckOptions& options)
		: m(std::make_unique<Impl>())
	{
		m->Options = options;
		std::error_code ec;
		m->Files.StartDir = fs::current_path(ec);
		for (const std::string& dir : m->Options.SearchPath)
			m->Files.SearchDirs.push_back(m->Files.StartDir / fs::u8path(dir));
		m->Files.HostTypes = &m->Options.Modules;
		m->Files.HostSources = &m->Options.ModuleSources;
		m->Modes.Modules = &m->Files;
		Luau::FrontendOptions frontendOptions;
		frontendOptions.retainFullTypeGraphs = true; // the strict rules read the inferred types
		// Luau's original type solver: the new one (Luau 0.748) does not yet
		// find a subclass's inherited methods through its intersection type,
		// and misreads pcall's arguments and optional option tables.
		m->Frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::Old, &m->Files, &m->Modes, frontendOptions);
		Luau::Frontend& frontend = *m->Frontend;
		for (const bool forAutocomplete : { false, true })
		{
			Luau::GlobalTypes& globals = forAutocomplete ? frontend.globalsForAutocomplete : frontend.globals;
			Luau::registerBuiltinGlobals(frontend, globals, forAutocomplete);
			Luau::LoadDefinitionFileResult loaded =
				frontend.loadDefinitionFile(globals, globals.globalScope, kGlobals, "@cluau", false, forAutocomplete);
			m->GlobalsLoaded = m->GlobalsLoaded && loaded.success;
			// getmetatable takes any value, as it does when the script runs:
			// Luau's original solver has it for tables with a metatable only.
			// Those still give their metatable's type; other values unknown.
			if (Luau::Binding* binding = Luau::tryGetGlobalBindingRef(globals, "getmetatable"))
			{
				const Luau::TypeId unknown = frontend.builtinTypes->unknownType;
				binding->typeId = Luau::makeIntersection(
					globals.globalTypes, { binding->typeId, Luau::makeFunction(globals.globalTypes, std::nullopt, { unknown }, { unknown }) });
			}
			Luau::freeze(globals.globalTypes);
		}
	}

	detail::Analysis::~Analysis() = default;

	Luau::Frontend& detail::Analysis::Frontend() { return *m->Frontend; }

	bool detail::Analysis::GlobalsLoaded() const { return m->GlobalsLoaded; }

	std::string detail::Analysis::AddRoot(const fs::path& file)
	{
		const std::string name = detail::ModuleKey(file);
		m->Files.MainDirs[name] = fs::u8path(name).parent_path();
		m->Files.RootOf[name] = name;
		return name;
	}

	void detail::Analysis::SetBuffer(const std::string& name, std::optional<std::string> text)
	{
		if (text)
			m->Files.Buffers[name] = std::move(*text);
		else
			m->Files.Buffers.erase(name);
		m->Frontend->markDirty(name);
	}

	void detail::Analysis::MarkDirty(const std::string& name)
	{
		m->Frontend->markDirty(name);
		// A require that named no file may name this one now: the modules
		// whose requires failed resolve them again.
		std::set<std::string> unresolved;
		for (const auto& [at, why] : m->Files.NotFound)
			unresolved.insert(at.first);
		for (const std::string& module : unresolved)
			m->Frontend->markDirty(module);
	}

	std::optional<std::string> detail::Analysis::TextOf(const std::string& name) const
	{
		if (auto it = m->Files.Texts.find(name); it != m->Files.Texts.end() && !m->Frontend->isDirty(name))
			return it->second;
		return m->Files.TextOf(name);
	}

	const TranslateResult* detail::Analysis::Translation(const std::string& name) const
	{
		auto it = m->Files.Translations.find(name);
		return it == m->Files.Translations.end() ? nullptr : &it->second;
	}

	const std::vector<std::string>& detail::Analysis::UserModules() const { return m->Files.UserModules; }

	std::string detail::Analysis::Display(const std::string& name) const { return m->Files.Display(name); }

	std::optional<std::string> detail::Analysis::Resolve(const std::string& from, const std::string& request)
	{
		Luau::AstExprConstantString expr(Luau::Location{}, Luau::AstArray<char>{ const_cast<char*>(request.data()), request.size() },
			Luau::AstExprConstantString::QuoteStyle::QuotedSimple);
		const Luau::ModuleInfo context{ from };
		Luau::TypeCheckLimits limits;
		if (std::optional<Luau::ModuleInfo> found = m->Files.resolveModule(&context, &expr, limits))
			return found->name;
		return std::nullopt;
	}

	std::optional<std::string> detail::Analysis::ClassNameOf(Luau::TypeId type)
	{
		Luau::ToStringOptions structural;
		structural.ignoreSyntheticName = true;
		const std::string printed = Plain(Luau::toString(type, structural));
		for (const auto& [classType, className] : ClassTypeNames(*m->Frontend, m->Files))
			if (classType == printed)
				return className;
		return std::nullopt;
	}

	std::vector<CheckFinding> detail::Analysis::Findings(const std::string& name)
	{
		std::vector<CheckFinding> out;
		Luau::Frontend& frontend = *m->Frontend;
		const CheckOptions& options = m->Options;
		Files& files = m->Files;
		Luau::ModulePtr module = frontend.moduleResolver.getModule(name);

		const std::vector<std::pair<std::string, std::string>> classNames = ClassTypeNames(frontend, files);
		auto plain = Plain;
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
				for (const auto& [printed, className] : classNames)
				{
					const bool cut = quoted.size() > kCut.size() && quoted.compare(quoted.size() - kCut.size(), kCut.size(), kCut) == 0;
					const size_t length = cut ? quoted.size() - kCut.size() : quoted.size();
					if (cut ? printed.compare(0, length, quoted, 0, length) == 0 : printed == quoted)
					{
						message.replace(open + 1, close - open - 1, className + suffix);
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
				return out; // the translation's Luau is not the script: its errors would mislead
		}
		std::set<std::string> seen;
		std::set<int> unresolvedLines;
		if (module)
			for (const Luau::TypeError& e : module->errors)
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
		if (translation && !options.Loose && source && module)
			for (const check::Finding& c : check::Run(*source, *module, *translation))
			{
				// An unresolved require's value is any: the not-found error says it.
				if (c.Code == "NoAny" && unresolvedLines.count(translation->Map.ToLuau(c.At.Begin).Line))
					continue;
				out.push_back({ display, c.At, c.Code, c.Message, c.Warning });
			}
		std::stable_sort(out.begin(), out.end(), [](const CheckFinding& a, const CheckFinding& b) { return a.At.Begin < b.At.Begin; });
		return out;
	}

	std::vector<CheckFinding> Check(const std::vector<std::string>& paths, const CheckOptions& options)
	{
		std::vector<CheckFinding> out;
		detail::Analysis analysis(options);
		if (!analysis.GlobalsLoaded())
			out.push_back({ "<runtime globals>", {}, "Internal", "the runtime's global declarations do not load", false });
		std::error_code ec;
		const fs::path start = fs::current_path(ec);
		std::vector<std::string> roots;
		for (const std::string& path : paths)
		{
			const fs::path file = start / fs::u8path(path);
			if (!fs::is_regular_file(file, ec))
			{
				out.push_back({ path, {}, "Missing", "cannot read the file", false });
				continue;
			}
			roots.push_back(analysis.AddRoot(file));
		}
		for (const std::string& root : roots)
			analysis.Frontend().check(root);
		std::set<std::string> reported;
		for (const std::string& name : analysis.UserModules())
			if (reported.insert(name).second)
				for (CheckFinding& f : analysis.Findings(name))
					out.push_back(std::move(f));
		std::stable_sort(out.begin(), out.end(), [](const CheckFinding& a, const CheckFinding& b) {
			return a.File != b.File ? a.File < b.File : a.At.Begin < b.At.Begin;
		});
		return out;
	}

} // namespace cluau::runtime
