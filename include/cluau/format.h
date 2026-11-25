#pragma once

// cluau fmt - a .cluau file's layout made regular, every line kept on its
// line: each line is indented by the brackets open at its start (one level
// for a line that opens any, a closing line back at its opener's level, and
// one more for a statement continued from the line before), trailing
// whitespace is removed, and the file ends with one line break. Text inside
// strings, long strings and comments that span lines is left as written, so
// formatting never changes what a script does.

#include "cluau/cluau.h"

#include <string>
#include <string_view>
#include <vector>

namespace cluau {

	struct FormatOptions
	{
		std::string Indent = "\t"; // one level of indentation
	};

	struct FormatResult
	{
		std::string Text;                    // the source itself when it cannot be read (see Diagnostics)
		std::vector<Diagnostic> Diagnostics; // why the source was left as it is
		bool Changed = false;
		bool HasErrors() const;
	};

	FormatResult Format(std::string_view source, const FormatOptions& options = {});

} // namespace cluau
