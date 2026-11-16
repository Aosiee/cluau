// cluau - the command line: translate a .cluau file to Luau, or convert a
// Luau file to cluau (stdout), reporting diagnostics as
// `file:line:col: message` (stderr).
//
//   cluau translate [--manifest] <file.cluau>
//   cluau convert [--verify] <file.luau>

#include "cluau/cluau.h"
#include "cluau/convert.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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

	int Usage()
	{
		std::fprintf(stderr, "usage: cluau translate [--manifest] <file.cluau>\n       cluau convert [--verify] <file.luau>\n");
		return 2;
	}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
		return Usage();
	const std::string command = argv[1];
	bool manifest = false;
	bool verify = false;
	std::string path;
	for (int i = 2; i < argc; i++)
	{
		const std::string arg = argv[i];
		if (arg == "--manifest" && command == "translate")
			manifest = true;
		else if (arg == "--verify" && command == "convert")
			verify = true;
		else if (arg.rfind("--", 0) == 0)
			return Usage();
		else
			path = arg;
	}
	std::string text;
	if (path.empty())
		return Usage();
	if (!Read(path, text))
		return 2;
	if (command == "translate")
	{
		cluau::TranslateOptions options;
		options.Manifest = manifest;
		const cluau::TranslateResult result = cluau::Translate(text, options);
		Report(path, result.Diagnostics);
		std::cout << result.Luau;
		return result.HasErrors() ? 1 : 0;
	}
	if (command == "convert")
	{
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
	return Usage();
}
