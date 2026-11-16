#pragma once

// cluau_check - cluau's strict rules, run over Luau's analysis of a
// translated module (docs/syntax.md "Strict typing"): no `any` unless the
// line carries an `allow-any` exemption, parameters annotated unless their
// type comes from context, functions that return a value annotate their
// return type, `const` locals never assigned again, and an exemption that
// covers nothing is a warning. Strict mode itself is the host's Luau config.
//
// Built against the host's Luau (Analysis + Ast headers); the translator
// (cluau.h) does not need it.

#include "cluau/cluau.h"

#include <string>
#include <vector>

namespace Luau {
	struct SourceModule;
	struct Module;
}

namespace cluau::check {

	struct Finding
	{
		Range At;          // source positions (the translation's map applied)
		std::string Code;  // NoAny, AnnotateParameter, AnnotateReturn, ConstAssign, UnusedExemption
		std::string Message;
		bool Warning = false;
	};

	// `source` / `module` are Luau's analysis of `translation.Luau` (a check
	// with full type graphs retained).
	std::vector<Finding> Run(const Luau::SourceModule& source, const Luau::Module& module, const TranslateResult& translation);

} // namespace cluau::check
