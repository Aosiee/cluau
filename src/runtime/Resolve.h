#pragma once

// Where `require(name)` finds a file: shared by the runtime (which loads it)
// and the checker (which type-checks it), so both read the same module.

#include <filesystem>
#include <string>
#include <vector>

namespace cluau::runtime::detail {

	// The file `require(name)` names, or an empty path. "./x" and "../x"
	// resolve from `fromDir` (the requiring file's directory), an absolute
	// path is itself, and a bare name is looked for in `mainDir` (the main
	// script's directory) and then each of `searchDirs`. For each, the name
	// itself (when it has an extension), `.cluau`, `.luau`, `/init.cluau` and
	// `/init.luau` are tried. `looked` gets the bases tried (for an error).
	std::filesystem::path ResolveRequire(const std::string& name, const std::filesystem::path& fromDir, const std::filesystem::path& mainDir,
		const std::vector<std::filesystem::path>& searchDirs, std::vector<std::filesystem::path>* looked = nullptr);

	// A path's text with `/` separators on every platform.
	std::string PathText(const std::filesystem::path& path);

	// A file's identity: its canonical path's text (the path itself when it
	// cannot be made canonical).
	std::string ModuleKey(const std::filesystem::path& path);

	// True for a .luau / .lua file (run and checked as Luau, untranslated).
	bool IsLuauFile(const std::filesystem::path& path);

} // namespace cluau::runtime::detail
