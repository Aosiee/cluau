#pragma once

// The checker's machinery, shared by `cluau check` and the language server:
// one Luau frontend over scripts and the modules they require (resolved as
// `require` resolves them at run time), with the standard modules and the
// host's modules typed, the translations kept, and cluau's strict rules.
// Module names are files' canonical paths (ModuleKey), `@std/<name>` and the
// host's `@alias/name`.

#include "cluau/runtime.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Luau {
	struct Frontend;
	struct Type;
	using TypeId = const Type*;
}

namespace cluau::runtime::detail {

	class Analysis
	{
	public:
		explicit Analysis(const CheckOptions& options = {});
		~Analysis();
		Analysis(const Analysis&) = delete;
		Analysis& operator=(const Analysis&) = delete;

		Luau::Frontend& Frontend();
		// False when the runtime's global declarations did not load.
		bool GlobalsLoaded() const;

		// A file checked as a script (bare requires search its directory
		// first); returns its module name.
		std::string AddRoot(const std::filesystem::path& file);
		// Text that stands for a module's file (an editor's unsaved buffer);
		// nullopt goes back to the file. The module re-checks, and so do
		// those that require it.
		void SetBuffer(const std::string& name, std::optional<std::string> text);
		// The file changed on disk.
		void MarkDirty(const std::string& name);

		// A module's text as written (a .cluau file's, before translation).
		std::optional<std::string> TextOf(const std::string& name) const;
		// A .cluau module's translation, once it has been read.
		const TranslateResult* Translation(const std::string& name) const;
		// The files read, in the order they were.
		const std::vector<std::string>& UserModules() const;
		// A module's name as messages show it (relative to the working
		// directory when it is under it).
		std::string Display(const std::string& name) const;
		// The module `require(request)` in module `from` names.
		std::optional<std::string> Resolve(const std::string& from, const std::string& request);

		// The class a type is an instance of, when it is one (a class's type
		// prints as its whole structure otherwise).
		std::optional<std::string> ClassNameOf(Luau::TypeId type);

		// A checked module's findings, sorted by position.
		std::vector<CheckFinding> Findings(const std::string& name);

	private:
		struct Impl;
		std::unique_ptr<Impl> m;
	};

} // namespace cluau::runtime::detail
