// cluau - the command line. Runs scripts the way `python` does, and
// translates / converts files (stdout), reporting diagnostics as
// `file:line:col: message` (stderr).
//
//   cluau <script.cluau> [args...]          run a script (also: cluau run ...)
//   cluau -e '<code>' [args...]             run code from the command line
//   cluau - [args...]                       run the script on standard input
//   cluau [-i]                              the REPL (-i: even when stdin is not a terminal)
//   cluau check <script.cluau>...           type-check scripts without running them
//   cluau translate [--manifest|--script] <file.cluau>
//   cluau convert [--verify] <file.luau>

#include "cluau/cluau.h"
#include "cluau/convert.h"
#include "cluau/format.h"

#ifdef CLUAU_HAS_RUNTIME
#include "cluau/runtime.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define CLUAU_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define CLUAU_ISATTY(fd) isatty(fd)
#endif

namespace {

	bool Read(const std::string& path, std::string& text)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			std::fprintf(stderr, "cluau: cannot read '%s'\n", path.c_str());
			return false;
		}
		std::stringstream buffer;
		buffer << in.rdbuf();
		text = buffer.str();
		return true;
	}

	void Report(const std::string& path, const std::vector<cluau::Diagnostic>& diagnostics)
	{
		for (const cluau::Diagnostic& d : diagnostics)
			std::fprintf(stderr, "%s:%d:%d: %s%s\n", path.c_str(), d.At.Begin.Line + 1, d.At.Begin.Col + 1,
				d.Warning ? "warning: " : "", d.Message.c_str());
	}

	const char* kUsage =
		"usage: cluau [--sandbox] [--no-native] <script.cluau> [args...]\n"
		"                                           run a script (also: cluau run <script> [args...])\n"
		"       cluau -e '<code>' [args...]         run code given on the command line\n"
		"       cluau - [args...]                   run the script on standard input\n"
		"       cluau [-i]                          the interactive prompt (-i: even when stdin is not a terminal)\n"
		"       cluau test [-k <text>] [--coverage] [--lcov <file>] [path...]\n"
		"                                           run the @std/test cases in *_test.cluau files (no path: .);\n"
		"                                           --coverage reports the lines the tests did not run\n"
		"       cluau profile [--top <n>] [--folded <file>] <script.cluau> [args...]\n"
		"                                           run a script and report where its time went\n"
		"       cluau init | add <repository>[@ref] | remove <name> | install | update [name...] | list\n"
		"                                           packages from Git repositories (cluau.toml, cluau.lock, cluau_modules/)\n"
		"       cluau check [--loose] [--lint] [--module <@alias/name>=<file>]... <script.cluau>...\n"
		"                                           type-check scripts (and the modules they require) without running them\n"
		"       cluau lsp [--module <@alias/name>=<file>]...\n"
		"                                           the language server, for editors (on standard input and output)\n"
		"       cluau debug                         the debugger, for editors (the Debug Adapter Protocol, on standard\n"
		"                                           input and output)\n"
		"       cluau translate [--manifest|--script] <file.cluau>\n"
		"       cluau convert [--verify] <file.luau>\n"
		"       cluau fmt [--check] [--indent <text>] [path...]   format .cluau files (every one under a directory; no path: .)\n"
		"       cluau --version\n"
		"\n"
		"`cluau check` is Luau's type checker in strict mode plus cluau's strict rules (exit status 1 on an error);\n"
		"--loose leaves out cluau's strict rules, --lint adds Luau's lint warnings, --module types a host's module\n"
		"(a .luau file returning a value of its type, or the module's .cluau source).\n"
		"--sandbox runs without file, network or process access (no @std/fs, @std/http, or programs run);\n"
		"--no-native interprets the bytecode instead of compiling it to native code. Either goes before the\n"
		"command (cluau --sandbox -e '...', cluau --sandbox test).\n"
		"Scripts `require` .cluau / .luau files (\"./name\" from the requiring file, a bare name from the\n"
		"script's directory and then CLUAU_PATH) and the standard modules (@std/<name>).\n";

	bool g_Sandbox = false; // --sandbox
	bool g_Native = true;   // --no-native turns it off

	int Usage(int status = 2)
	{
		std::FILE* out = status == 0 ? stdout : stderr;
		std::fputs(kUsage, out);
#ifdef CLUAU_HAS_RUNTIME
		std::fprintf(out, "This build's standard modules: %s.\n", cluau::runtime::StandardModules());
#endif
		return status;
	}

	int Translate(int argc, char** argv)
	{
		bool manifest = false;
		bool script = false;
		std::string path;
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if (arg == "--manifest")
				manifest = true;
			else if (arg == "--script")
				script = true;
			else if (arg.rfind("--", 0) == 0 || !path.empty())
				return Usage();
			else
				path = arg;
		}
		std::string text;
		if (path.empty())
			return Usage();
		if (!Read(path, text))
			return 2;
		cluau::TranslateOptions options;
		options.Manifest = manifest;
		options.Script = script;
		if (script)
			options.Profile = &cluau::ScriptProfile();
		const cluau::TranslateResult result = cluau::Translate(text, options);
		Report(path, result.Diagnostics);
		std::cout << result.Luau;
		return result.HasErrors() ? 1 : 0;
	}

	// cluau fmt: each file formatted in place (`--check`: only reported, and
	// the exit status 1 when one would change); `-` formats standard input
	// to standard output.
	int Fmt(int argc, char** argv)
	{
		namespace fs = std::filesystem;
		bool check = false;
		cluau::FormatOptions options;
		std::vector<std::string> paths;
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if (arg == "--check")
				check = true;
			else if (arg == "--indent")
			{
				if (++i >= argc)
					return Usage();
				options.Indent = argv[i];
			}
			else if (arg.rfind("--", 0) == 0)
				return Usage();
			else
				paths.push_back(arg);
		}
		if (paths.size() == 1 && paths[0] == "-")
		{
			std::stringstream buffer;
			buffer << std::cin.rdbuf();
			const cluau::FormatResult result = cluau::Format(buffer.str(), options);
			Report("stdin", result.Diagnostics);
			std::cout << result.Text;
			return result.HasErrors() ? 1 : 0;
		}
		if (paths.empty())
			paths.push_back(".");

		// The files: those named, and every .cluau under a directory named
		// (hidden directories left out), in order.
		std::vector<std::string> files;
		for (const std::string& path : paths)
		{
			std::error_code ec;
			if (fs::is_directory(fs::u8path(path), ec))
			{
				std::vector<std::string> found;
				fs::recursive_directory_iterator it(fs::u8path(path), ec), end;
				for (; !ec && it != end; it.increment(ec))
				{
					const std::string name = it->path().filename().u8string();
					if (it->is_directory(ec) && ((!name.empty() && name[0] == '.' && name != "." && name != "..") || name == "cluau_modules"))
					{
						it.disable_recursion_pending();
						continue;
					}
					if (it->is_regular_file(ec) && cluau::IsCluauPath(name))
						found.push_back(it->path().lexically_normal().generic_u8string());
				}
				std::sort(found.begin(), found.end());
				files.insert(files.end(), found.begin(), found.end());
			}
			else
				files.push_back(path);
		}

		int changed = 0, failed = 0;
		for (const std::string& file : files)
		{
			std::string text;
			if (!Read(file, text))
			{
				failed++;
				continue;
			}
			const cluau::FormatResult result = cluau::Format(text, options);
			if (result.HasErrors())
			{
				Report(file, result.Diagnostics);
				failed++;
				continue;
			}
			if (!result.Changed)
				continue;
			changed++;
			if (check)
			{
				std::printf("%s needs formatting\n", file.c_str());
				continue;
			}
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			out << result.Text;
			if (!out)
			{
				std::fprintf(stderr, "cluau: cannot write '%s'\n", file.c_str());
				failed++;
				continue;
			}
			std::printf("formatted %s\n", file.c_str());
		}
		if (check && changed)
			std::printf("%d of %zu file(s) need formatting (cluau fmt formats them)\n", changed, files.size());
		return failed || (check && changed) ? 1 : 0;
	}

	int Convert(int argc, char** argv)
	{
		bool verify = false;
		std::string path;
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if (arg == "--verify")
				verify = true;
			else if (arg.rfind("--", 0) == 0 || !path.empty())
				return Usage();
			else
				path = arg;
		}
		std::string text;
		if (path.empty())
			return Usage();
		if (!Read(path, text))
			return 2;
		const cluau::ConvertResult result = cluau::Convert(text);
		Report(path, result.Diagnostics);
		if (verify)
		{
			const std::string problem = cluau::VerifyConversion(text, result.Cluau);
			if (!problem.empty())
			{
				std::fprintf(stderr, "%s: round trip: %s\n", path.c_str(), problem.c_str());
				return 1;
			}
		}
		std::cout << result.Cluau;
		return result.HasErrors() ? 1 : 0;
	}

#ifdef CLUAU_HAS_RUNTIME
	std::vector<std::string> SearchPath()
	{
		std::vector<std::string> dirs;
		const char* env = std::getenv("CLUAU_PATH");
		if (!env)
			return dirs;
#ifdef _WIN32
		const char sep = ';';
#else
		const char sep = ':';
#endif
		std::string item;
		for (const char* c = env;; c++)
		{
			if (*c == sep || *c == '\0')
			{
				if (!item.empty())
					dirs.push_back(item);
				item.clear();
				if (*c == '\0')
					break;
			}
			else
				item += *c;
		}
		return dirs;
	}

	// --module @game/world=types/world.luau: a host's module typed from a
	// file (a .cluau one from its source). Nonzero: the exit status.
	int ModuleOption(const std::string& spec, cluau::runtime::CheckOptions& options)
	{
		const size_t eq = spec.find('=');
		if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size())
			return Usage();
		const std::string path = spec.substr(eq + 1);
		std::string text;
		if (!Read(path, text))
			return 2;
		const bool cluauSource = path.size() > 6 && path.compare(path.size() - 6, 6, ".cluau") == 0;
		(cluauSource ? options.ModuleSources : options.Modules)[spec.substr(0, eq)] = text;
		return 0;
	}

	// cluau lsp: the language server, on standard input and output.
	int Lsp(int argc, char** argv)
	{
		cluau::runtime::CheckOptions options;
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if (arg == "--module")
			{
				// A file that cannot be read leaves its module out: the
				// server starts all the same (an editor's stale setting).
				const std::string spec = ++i < argc ? argv[i] : "";
				const size_t eq = spec.find('=');
				if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size())
					return Usage();
				if (ModuleOption(spec, options) != 0)
					std::fprintf(stderr, "cluau lsp: serving without the module %s\n", spec.substr(0, eq).c_str());
			}
			else if (arg == "--stdio")
				continue; // how editors' clients ask for standard input and output
			else
				return Usage();
		}
		options.SearchPath = SearchPath();
		return cluau::runtime::ServeLanguageServer(options);
	}

	// cluau debug: the debug adapter, on standard input and output. The
	// editor's launch request names the script.
	int Debug(int argc, char** argv)
	{
		for (int i = 2; i < argc; i++)
			if (std::string(argv[i]) != "--stdio")
				return Usage();
		cluau::runtime::Options options;
		options.SearchPath = SearchPath();
		options.Sandbox = g_Sandbox;
		return cluau::runtime::ServeDebugAdapter(options);
	}

	int Check(int argc, char** argv)
	{
		std::vector<std::string> files;
		cluau::runtime::CheckOptions options;
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if (arg == "--loose")
				options.Loose = true;
			else if (arg == "--lint")
				options.Lint = true;
			else if (arg == "--module")
			{
				if (const int status = ModuleOption(++i < argc ? argv[i] : "", options))
					return status;
			}
			else if (arg[0] == '-' && arg.size() > 1)
				return Usage();
			else
				files.push_back(arg);
		}
		if (files.empty())
			return Usage();
		options.SearchPath = SearchPath();
		int errors = 0, warnings = 0;
		for (const cluau::runtime::CheckFinding& f : cluau::runtime::Check(files, options))
		{
			std::fprintf(stderr, "%s:%d:%d: %s%s\n", f.File.c_str(), f.At.Begin.Line + 1, f.At.Begin.Col + 1, f.Warning ? "warning: " : "",
				f.Message.c_str());
			(f.Warning ? warnings : errors)++;
		}
		if (errors || warnings)
			std::fprintf(stderr, "cluau check: %d error(s), %d warning(s)\n", errors, warnings);
		return errors ? 1 : 0;
	}

	cluau::runtime::Options RunOptions(int argc, char** argv, int firstArg);

	// cluau test: every *_test.cluau under the paths (and files named), each
	// in a runtime of its own, then a summary. The exit status is 1 when a
	// case failed, a file did not run, or there was nothing to run.
	// The coverage of the files the tests ran (not the test files, nor
	// packages): each file's lines run of its lines with code, and the
	// lines not run; LCOV to `lcov` when it names a file. False when that
	// file cannot be written.
	bool ReportCoverage(const std::map<std::string, std::map<int, int>>& lines, const std::string& lcov)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path cwd = fs::current_path(ec);
		struct Row
		{
			std::string Shown, File;
			int Run = 0, Total = 0;
			std::string Missed;
		};
		std::vector<Row> rows;
		for (const auto& [file, hits] : lines)
		{
			const std::string generic = fs::u8path(file).generic_u8string();
			if ((generic.size() > 11 && generic.compare(generic.size() - 11, 11, "_test.cluau") == 0) ||
				generic.find("/cluau_modules/") != std::string::npos || hits.empty())
				continue;
			Row row;
			row.File = file;
			const fs::path rel = fs::u8path(file).lexically_relative(cwd);
			row.Shown = !rel.empty() && *rel.begin() != ".." ? rel.generic_u8string() : generic;
			// The lines not run, as ranges ("7-9, 14"): runs of missed lines
			// with no line of code between them that ran.
			int from = 0, to = 0;
			auto flush = [&] {
				if (!from)
					return;
				row.Missed += (row.Missed.empty() ? "" : ", ") + std::to_string(from) + (to > from ? "-" + std::to_string(to) : "");
				from = 0;
			};
			for (const auto& [line, count] : hits)
			{
				row.Total++;
				if (count > 0)
				{
					row.Run++;
					flush();
				}
				else if (from)
					to = line;
				else
					from = to = line;
			}
			flush();
			rows.push_back(std::move(row));
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.Shown < b.Shown; });
		size_t width = 5;
		for (const Row& row : rows)
			width = std::max(width, row.Shown.size());
		int run = 0, total = 0;
		std::printf("\ncoverage (lines run):\n");
		for (const Row& row : rows)
		{
			run += row.Run;
			total += row.Total;
			std::printf("  %-*s  %5d/%-5d %3d%%%s%s\n", (int)width, row.Shown.c_str(), row.Run, row.Total, row.Total ? 100 * row.Run / row.Total : 100,
				row.Missed.empty() ? "" : "   not run: ", row.Missed.c_str());
		}
		std::printf("  %-*s  %5d/%-5d %3d%%\n", (int)width, "total", run, total, total ? 100 * run / total : 100);
		if (lcov.empty())
			return true;
		std::ofstream out(fs::u8path(lcov), std::ios::binary);
		for (const Row& row : rows)
		{
			out << "TN:\nSF:" << row.File << "\n";
			for (const auto& [line, count] : lines.at(row.File))
				out << "DA:" << line << "," << count << "\n";
			out << "LF:" << row.Total << "\nLH:" << row.Run << "\nend_of_record\n";
		}
		if (!out)
		{
			std::fprintf(stderr, "cluau: cannot write '%s'\n", lcov.c_str());
			return false;
		}
		return true;
	}

	// cluau profile: runs the script, sampling where its time goes.
	int Profile(int argc, char** argv)
	{
		int at = 2;
		int top = 20;
		std::string folded;
		for (; at < argc; at++)
		{
			const std::string arg = argv[at];
			if (arg == "--top" && at + 1 < argc)
				top = std::max(1, std::atoi(argv[++at]));
			else if (arg == "--folded" && at + 1 < argc)
				folded = argv[++at];
			else if (arg.rfind("-", 0) == 0)
				return Usage();
			else
				break;
		}
		if (at >= argc)
			return Usage();
		cluau::runtime::Options options = RunOptions(argc, argv, at + 1);
		options.Profile = true;
		options.ProfileTop = top;
		options.ProfileFolded = folded;
		// The file is written at the end: one that cannot be is found now,
		// before the script runs.
		if (!folded.empty() && !std::ofstream(std::filesystem::u8path(folded), std::ios::binary))
		{
			std::fprintf(stderr, "cluau: cannot write '%s'\n", folded.c_str());
			return 2;
		}
		cluau::runtime::Runtime runtime(options);
		return runtime.RunFile(argv[at]);
	}

	int Test(int argc, char** argv)
	{
		namespace fs = std::filesystem;
		std::string filter;
		std::vector<std::string> paths;
		bool coverage = false;
		std::string lcov;
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if (arg == "-k")
			{
				if (++i >= argc)
					return Usage();
				filter = argv[i];
			}
			else if (arg == "--coverage")
				coverage = true;
			else if (arg == "--lcov")
			{
				if (++i >= argc)
					return Usage();
				lcov = argv[i];
				coverage = true;
			}
			else if (arg.rfind("-", 0) == 0)
				return Usage();
			else
				paths.push_back(arg);
		}
		if (paths.empty())
			paths.push_back(".");
		auto isTestFile = [](const std::string& name) {
			return name.size() > 11 && name.compare(name.size() - 11, 11, "_test.cluau") == 0;
		};
		std::vector<std::string> files;
		for (const std::string& path : paths)
		{
			std::error_code ec;
			if (!fs::is_directory(fs::u8path(path), ec))
			{
				files.push_back(path);
				continue;
			}
			std::vector<std::string> found;
			fs::recursive_directory_iterator it(fs::u8path(path), ec), end;
			for (; !ec && it != end; it.increment(ec))
			{
				const std::string name = it->path().filename().u8string();
				if (it->is_directory(ec) && ((!name.empty() && name[0] == '.' && name != "." && name != "..") || name == "cluau_modules"))
				{
					it.disable_recursion_pending();
					continue;
				}
				if (it->is_regular_file(ec) && isTestFile(name))
				{
					std::string shown = it->path().lexically_normal().generic_u8string();
					found.push_back(shown);
				}
			}
			std::sort(found.begin(), found.end());
			files.insert(files.end(), found.begin(), found.end());
		}
		if (files.empty())
		{
			std::printf("no test files (*_test.cluau) under %s\n", paths.size() == 1 ? paths[0].c_str() : "the paths given");
			return 1;
		}
		int passed = 0, failed = 0, skipped = 0, broken = 0;
		std::map<std::string, std::map<int, int>> lines; // the lines run, by file, over every test file
		for (const std::string& file : files)
		{
			std::printf("%s\n", file.c_str());
			std::fflush(stdout);
			cluau::runtime::Options options = RunOptions(argc, argv, argc);
			options.Coverage = coverage;
			cluau::runtime::Runtime runtime(options);
			cluau::runtime::Runtime::TestCounts counts;
			if (!runtime.RunTest(file, filter, counts))
				broken++;
			if (coverage)
				for (const auto& [covered, hits] : runtime.Coverage())
					for (const auto& [line, count] : hits)
						lines[covered][line] += count;
			passed += counts.Passed;
			failed += counts.Failed;
			skipped += counts.Skipped;
			std::fflush(stdout);
		}
		std::string summary = std::to_string(passed) + " passed";
		if (failed)
			summary += ", " + std::to_string(failed) + " failed";
		if (skipped)
			summary += ", " + std::to_string(skipped) + " skipped";
		if (broken)
			summary += ", " + std::to_string(broken) + " file(s) did not finish";
		std::printf("%s (%zu file(s))\n", summary.c_str(), files.size());
		if (coverage && !ReportCoverage(lines, lcov))
			return 1;
		return failed || broken || passed + skipped == 0 ? 1 : 0;
	}

	cluau::runtime::Options RunOptions(int argc, char** argv, int firstArg)
	{
		cluau::runtime::Options options;
		for (int i = firstArg; i < argc; i++)
			options.Args.push_back(argv[i]);
		options.SearchPath = SearchPath();
		options.CatchInterrupt = true;
		options.Sandbox = g_Sandbox;
		options.Native = g_Native;
		return options;
	}
#endif

	[[maybe_unused]] int NoRuntime()
	{
		std::fprintf(stderr, "cluau: this build has no runtime (it was built without Luau): only translate and convert are available\n");
		return 2;
	}

} // namespace

int main(int argc, char** argv)
{
	// The runtime's flags, before the command: taken out of the arguments.
	std::vector<char*> rest{ argv[0] };
	int at = 1;
	for (; at < argc; at++)
	{
		const std::string arg = argv[at];
		if (arg == "--sandbox")
			g_Sandbox = true;
		else if (arg == "--no-native")
			g_Native = false;
		else
			break;
	}
	rest.insert(rest.end(), argv + at, argv + argc);
	rest.push_back(nullptr);
	argc = (int)rest.size() - 1;
	argv = rest.data();

	const std::string first = argc > 1 ? argv[1] : "";
	if (first == "translate")
		return Translate(argc, argv);
	if (first == "convert")
		return Convert(argc, argv);
	if (first == "fmt")
		return Fmt(argc, argv);
	if (first == "--help" || first == "-h")
		return Usage(0);
	if (first == "--version" || first == "-v")
	{
		std::printf("cluau %s\n", cluau::Version());
		return 0;
	}
#ifdef CLUAU_HAS_RUNTIME
	if (first == "check")
		return Check(argc, argv);
	if (first == "lsp")
		return Lsp(argc, argv);
	if (first == "debug")
		return Debug(argc, argv);
	if (first == "test")
		return Test(argc, argv);
	if (first == "profile")
		return Profile(argc, argv);
	if (first == "init" || first == "add" || first == "remove" || first == "install" || first == "update" || first == "list")
	{
		// The package commands are written in cluau (src/runtime/std/_packages.cluau).
		cluau::runtime::Runtime runtime(RunOptions(argc, argv, 1));
		return runtime.RunSource("require(\"@std/_packages\").main(require(\"@std/process\").args)", "cluau");
	}
	if (first.empty() || first == "-i")
	{
		if (first.empty() && !CLUAU_ISATTY(0))
		{
			// `echo 'print(1)' | cluau` runs standard input, as python does.
			std::stringstream buffer;
			buffer << std::cin.rdbuf();
			cluau::runtime::Runtime runtime(RunOptions(argc, argv, 1));
			return runtime.RunSource(buffer.str(), "stdin");
		}
		cluau::runtime::Runtime runtime(RunOptions(argc, argv, 2));
		return runtime.Repl();
	}
	if (first == "-e")
	{
		if (argc < 3)
			return Usage();
		cluau::runtime::Runtime runtime(RunOptions(argc, argv, 3));
		return runtime.RunSource(argv[2], "-e");
	}
	if (first == "-")
	{
		std::stringstream buffer;
		buffer << std::cin.rdbuf();
		cluau::runtime::Runtime runtime(RunOptions(argc, argv, 2));
		return runtime.RunSource(buffer.str(), "stdin");
	}
	const bool run = first == "run";
	const int scriptArg = run ? 2 : 1;
	if (scriptArg >= argc)
		return Usage();
	const std::string script = argv[scriptArg];
	if (!run && script.rfind("-", 0) == 0)
		return Usage();
	cluau::runtime::Runtime runtime(RunOptions(argc, argv, scriptArg + 1));
	return runtime.RunFile(script);
#else
	if (first.empty())
		return Usage();
	return NoRuntime();
#endif
}
