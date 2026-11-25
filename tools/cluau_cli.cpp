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

#ifdef CLUAU_HAS_RUNTIME
#include "cluau/runtime.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
		"usage: cluau <script.cluau> [args...]      run a script (also: cluau run <script> [args...])\n"
		"       cluau -e '<code>' [args...]         run code given on the command line\n"
		"       cluau - [args...]                   run the script on standard input\n"
		"       cluau [-i]                          the interactive prompt (-i: even when stdin is not a terminal)\n"
		"       cluau check <script.cluau>...       type-check scripts (and the modules they require) without running them\n"
		"       cluau translate [--manifest|--script] <file.cluau>\n"
		"       cluau convert [--verify] <file.luau>\n"
		"       cluau --version\n"
		"\n"
		"`cluau check` is Luau's type checker in strict mode plus cluau's strict rules (exit status 1 on an error).\n"
		"Scripts `require` .cluau / .luau files (\"./name\" from the requiring file, a bare name from the\n"
		"script's directory and then CLUAU_PATH) and the standard modules @std/fs, @std/path,\n"
		"@std/process, @std/io and @std/json.\n";

	int Usage(int status = 2)
	{
		std::fputs(kUsage, status == 0 ? stdout : stderr);
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

	int Check(int argc, char** argv)
	{
		std::vector<std::string> files;
		for (int i = 2; i < argc; i++)
		{
			if (argv[i][0] == '-' && argv[i][1] != '\0')
				return Usage();
			files.push_back(argv[i]);
		}
		if (files.empty())
			return Usage();
		cluau::runtime::CheckOptions options;
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

	cluau::runtime::Options RunOptions(int argc, char** argv, int firstArg)
	{
		cluau::runtime::Options options;
		for (int i = firstArg; i < argc; i++)
			options.Args.push_back(argv[i]);
		options.SearchPath = SearchPath();
		options.CatchInterrupt = true;
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
	const std::string first = argc > 1 ? argv[1] : "";
	if (first == "translate")
		return Translate(argc, argv);
	if (first == "convert")
		return Convert(argc, argv);
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
