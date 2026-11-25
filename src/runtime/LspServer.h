#pragma once

// The Language Server Protocol over the language service: what `cluau lsp`
// speaks on standard input and output. Handle takes one JSON-RPC message
// and returns the messages to send back (the response first, then any
// notifications); Serve frames them with Content-Length headers.
//
// LSP counts columns in UTF-16 code units, the service in bytes: each is
// converted through its line's text. Documents sync whole.

#include "Json.h"
#include "LanguageService.h"

#include <cstdio>
#include <string>
#include <vector>

namespace cluau::runtime::detail {

	class LspServer
	{
	public:
		explicit LspServer(const CheckOptions& options = {});

		std::vector<std::string> Handle(const std::string& message);

		// `exit` arrived: stop, with 0 when `shutdown` came first (LSP's rule).
		bool ShouldExit() const { return m_Exit; }
		int ExitCode() const { return m_ShutDown ? 0 : 1; }

		// Reads framed messages from `in` and writes the replies to `out`
		// until `exit` or the end of the input; returns the exit status.
		int Serve(std::FILE* in, std::FILE* out);

		static std::string PathFromUri(const std::string& uri);
		static std::string UriFromPath(const std::string& path);

		LanguageService& Service() { return m_Service; }

	private:
		Json Position(const std::string& path, cluau::Position at) const;
		Json RangeOf(const std::string& path, const Range& range) const;
		std::string LineAt(const std::string& path, int line) const;
		void Publish(std::vector<std::string>& out);

		LanguageService m_Service;
		std::vector<std::string> m_Published; // files given diagnostics (a closed one is cleared)
		bool m_ShutDown = false;
		bool m_Exit = false;
	};

} // namespace cluau::runtime::detail
