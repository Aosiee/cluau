#include "LspServer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace cluau::runtime::detail {

	namespace {

		std::string LineOfText(const std::string& text, int line)
		{
			size_t start = 0;
			for (int i = 0; i < line; i++)
			{
				start = text.find('\n', start);
				if (start == std::string::npos)
					return {};
				start++;
			}
			const size_t end = text.find('\n', start);
			std::string out = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!out.empty() && out.back() == '\r')
				out.pop_back();
			return out;
		}

		// UTF-16 code units <-> bytes on one line.
		int ByteToUtf16(const std::string& line, int byte)
		{
			int units = 0;
			int i = 0;
			while (i < byte && i < (int)line.size())
			{
				const unsigned char c = (unsigned char)line[(size_t)i];
				const int length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
				units += length == 4 ? 2 : 1;
				i += length;
			}
			return units + std::max(0, byte - (int)line.size());
		}

		int Utf16ToByte(const std::string& line, int units)
		{
			int i = 0;
			while (units > 0 && i < (int)line.size())
			{
				const unsigned char c = (unsigned char)line[(size_t)i];
				const int length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
				units -= length == 4 ? 2 : 1;
				i += length;
			}
			return std::min(i, (int)line.size());
		}

		int CompletionKindOf(CompletionKind kind)
		{
			switch (kind)
			{
			case CompletionKind::Variable: return 6;
			case CompletionKind::Function: return 3;
			case CompletionKind::Method: return 2;
			case CompletionKind::Property: return 10;
			case CompletionKind::Keyword: return 14;
			case CompletionKind::Type: return 7;
			case CompletionKind::Module: return 9;
			case CompletionKind::Value: return 12;
			}
			return 1;
		}

		int SymbolKindOf(SymbolKind kind)
		{
			switch (kind)
			{
			case SymbolKind::Class: return 5;
			case SymbolKind::Field: return 8;
			case SymbolKind::Method: return 6;
			case SymbolKind::Function: return 12;
			case SymbolKind::Variable: return 13;
			case SymbolKind::Enum: return 10;
			}
			return 13;
		}

		bool Readable(const std::string& path)
		{
			const std::string ext = std::filesystem::u8path(path).extension().u8string();
			return ext == ".cluau" || ext == ".luau" || ext == ".lua";
		}

	} // namespace

	LspServer::LspServer(const CheckOptions& options)
		: m_Service(options)
	{
	}

	std::string LspServer::PathFromUri(const std::string& uri)
	{
		std::string path = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
		std::string out;
		for (size_t i = 0; i < path.size(); i++)
		{
			if (path[i] == '%' && i + 2 < path.size() && std::isxdigit((unsigned char)path[i + 1]) && std::isxdigit((unsigned char)path[i + 2]))
			{
				out += (char)std::stoi(path.substr(i + 1, 2), nullptr, 16);
				i += 2;
			}
			else
				out += path[i];
		}
		// file:///c:/dir: a Windows drive.
		if (out.size() > 2 && out[0] == '/' && std::isalpha((unsigned char)out[1]) && out[2] == ':')
			out.erase(0, 1);
		return out;
	}

	std::string LspServer::UriFromPath(const std::string& path)
	{
		std::string generic = path;
		std::replace(generic.begin(), generic.end(), '\\', '/');
		std::string out = "file://";
		if (generic.empty() || generic[0] != '/')
			out += '/';
		for (char c : generic)
		{
			if (std::isalnum((unsigned char)c) || std::strchr("/-_.~:", c))
				out += c;
			else
			{
				char buffer[4];
				std::snprintf(buffer, sizeof(buffer), "%%%02X", (unsigned char)c);
				out += buffer;
			}
		}
		return out;
	}

	std::string LspServer::LineAt(const std::string& path, int line) const
	{
		std::optional<std::string> text = m_Service.TextOf(path);
		return text ? LineOfText(*text, line) : std::string();
	}

	Json LspServer::Position(const std::string& path, cluau::Position at) const
	{
		return Json::Object().Set("line", at.Line).Set("character", ByteToUtf16(LineAt(path, at.Line), at.Col));
	}

	Json LspServer::RangeOf(const std::string& path, const Range& range) const
	{
		return Json::Object().Set("start", Position(path, range.Begin)).Set("end", Position(path, range.End));
	}

	void LspServer::Publish(std::vector<std::string>& out)
	{
		const std::map<std::string, std::vector<CheckFinding>> all = m_Service.Diagnostics();
		// A file closed since: its diagnostics go.
		for (const std::string& path : m_Published)
			if (!all.count(path))
			{
				Json params = Json::Object().Set("uri", UriFromPath(path)).Set("diagnostics", Json::Array());
				out.push_back(
					Json::Object().Set("jsonrpc", "2.0").Set("method", "textDocument/publishDiagnostics").Set("params", std::move(params)).Dump());
			}
		m_Published.clear();
		for (const auto& [path, findings] : all)
		{
			m_Published.push_back(path);
			Json list = Json::Array();
			for (const CheckFinding& f : findings)
			{
				Range at = f.At;
				// A point widens to the word there.
				if (at.End.Line == at.Begin.Line && at.End.Col <= at.Begin.Col)
				{
					const std::string line = LineAt(path, at.Begin.Line);
					int end = at.Begin.Col;
					while (end < (int)line.size() && (std::isalnum((unsigned char)line[(size_t)end]) || line[(size_t)end] == '_'))
						end++;
					at.End = { at.Begin.Line, std::max(end, at.Begin.Col + 1) };
				}
				list.Push(Json::Object()
						.Set("range", RangeOf(path, at))
						.Set("severity", f.Warning ? 2 : 1)
						.Set("code", f.Code)
						.Set("source", "cluau")
						.Set("message", f.Message));
			}
			Json params = Json::Object().Set("uri", UriFromPath(path)).Set("diagnostics", std::move(list));
			out.push_back(Json::Object().Set("jsonrpc", "2.0").Set("method", "textDocument/publishDiagnostics").Set("params", std::move(params)).Dump());
		}
	}

	std::vector<std::string> LspServer::Handle(const std::string& text)
	{
		std::vector<std::string> out;
		std::optional<Json> parsed = Json::Parse(text);
		if (!parsed || !(*parsed)["method"].IsString())
			return out;
		const Json& request = *parsed;
		const std::string method = request["method"].String();
		const bool isRequest = request.Has("id");
		const Json& params = request["params"];

		Json result;
		std::string error;
		int errorCode = 0;
		const std::string path = PathFromUri(params["textDocument"]["uri"].String());
		// The request's position, in the service's bytes.
		const int line = params["position"]["line"].Int();
		const int col = Utf16ToByte(LineAt(path, line), params["position"]["character"].Int());

		if (method == "initialize")
		{
			Json capabilities = Json::Object();
			capabilities.Set("textDocumentSync", Json::Object().Set("openClose", true).Set("change", 1).Set("save", true));
			capabilities.Set("completionProvider", Json::Object().Set("triggerCharacters", Json::Array().Push(".").Push(":")));
			capabilities.Set("signatureHelpProvider", Json::Object().Set("triggerCharacters", Json::Array().Push("(").Push(",")));
			for (const char* provider : { "hoverProvider", "definitionProvider", "referencesProvider", "documentHighlightProvider",
					 "documentSymbolProvider", "documentFormattingProvider" })
				capabilities.Set(provider, true);
			capabilities.Set("renameProvider", true);
			capabilities.Set("workspaceSymbolProvider", true);
			capabilities.Set("inlayHintProvider", true);
			// Where references, renames and symbols look beyond the open files.
			std::vector<std::string> folders;
			for (const Json& folder : params["workspaceFolders"].Items())
				folders.push_back(PathFromUri(folder["uri"].String()));
			if (folders.empty() && params["rootUri"].IsString())
				folders.push_back(PathFromUri(params["rootUri"].String()));
			m_Service.SetWorkspace(std::move(folders));
			result = Json::Object()
						 .Set("capabilities", std::move(capabilities))
						 .Set("serverInfo", Json::Object().Set("name", "cluau").Set("version", cluau::Version()));
		}
		else if (method == "shutdown")
			m_ShutDown = true;
		else if (method == "exit")
			m_Exit = true;
		else if (method == "textDocument/didOpen")
		{
			if (Readable(path))
			{
				m_Service.Open(path, params["textDocument"]["text"].String());
				Publish(out);
			}
		}
		else if (method == "textDocument/didChange")
		{
			const Json& changes = params["contentChanges"];
			if (Readable(path) && changes.Size() > 0)
			{
				m_Service.Change(path, changes[changes.Size() - 1]["text"].String());
				Publish(out);
			}
		}
		else if (method == "textDocument/didClose")
		{
			if (m_Service.IsOpen(path))
			{
				m_Service.Close(path);
				Publish(out);
			}
		}
		else if (method == "textDocument/didSave")
		{
			// What required the saved file reads it again.
			m_Service.Changed(path);
			Publish(out);
		}
		else if (method == "workspace/didChangeWatchedFiles")
		{
			for (const Json& change : params["changes"].Items())
				m_Service.Changed(PathFromUri(change["uri"].String()));
			Publish(out);
		}
		else if (method == "textDocument/completion")
		{
			const CompletionList list = m_Service.Complete(path, line, col);
			result = Json::Array();
			for (const CompletionItem& item : list.Items)
			{
				Json entry = Json::Object().Set("label", item.Label).Set("kind", CompletionKindOf(item.Kind));
				if (!item.Detail.empty())
					entry.Set("detail", item.Detail);
				if (item.Deprecated)
					entry.Set("deprecated", true);
				const std::string insert = item.InsertText.empty() ? item.Label : item.InsertText;
				entry.Set("textEdit", Json::Object().Set("range", RangeOf(path, { { line, list.ReplaceFrom }, { line, col } })).Set("newText", insert));
				entry.Set("insertTextFormat", insert.find('$') != std::string::npos ? 2 : 1);
				result.Push(std::move(entry));
			}
		}
		else if (method == "textDocument/hover")
		{
			if (std::optional<HoverInfo> hover = m_Service.Hover(path, line, col))
			{
				std::string value = "```luau\n" + hover->Text + "\n```";
				if (!hover->Documentation.empty())
					value += "\n\n" + hover->Documentation;
				result = Json::Object()
							 .Set("contents", Json::Object().Set("kind", "markdown").Set("value", value))
							 .Set("range", RangeOf(path, hover->At));
			}
		}
		else if (method == "textDocument/signatureHelp")
		{
			if (std::optional<SignatureInfo> signature = m_Service.Signature(path, line, col))
			{
				Json parameters = Json::Array();
				for (const auto& [begin, end] : signature->Params)
					parameters.Push(Json::Object().Set(
						"label", Json::Array().Push(ByteToUtf16(signature->Label, begin)).Push(ByteToUtf16(signature->Label, end))));
				result = Json::Object()
							 .Set("signatures",
								 Json::Array().Push(Json::Object().Set("label", signature->Label).Set("parameters", std::move(parameters))))
							 .Set("activeSignature", 0)
							 .Set("activeParameter", std::max(0, signature->Active));
			}
		}
		else if (method == "textDocument/definition")
		{
			if (std::optional<Location> location = m_Service.Definition(path, line, col))
				result = Json::Object().Set("uri", UriFromPath(location->Path)).Set("range", RangeOf(location->Path, location->At));
		}
		else if (method == "textDocument/references" || method == "textDocument/documentHighlight")
		{
			const bool highlight = method == "textDocument/documentHighlight";
			const bool declaration = highlight || params["context"]["includeDeclaration"].Bool(true);
			result = Json::Array();
			const std::vector<Location> uses = m_Service.References(path, line, col);
			for (size_t i = declaration ? 0 : 1; i < uses.size(); i++)
			{
				// A highlight is the file's own uses.
				if (highlight && uses[i].Path != path)
					continue;
				if (highlight)
					result.Push(Json::Object().Set("range", RangeOf(path, uses[i].At)));
				else
					result.Push(Json::Object().Set("uri", UriFromPath(uses[i].Path)).Set("range", RangeOf(uses[i].Path, uses[i].At)));
			}
		}
		else if (method == "textDocument/rename")
		{
			std::string why;
			const std::map<std::string, std::vector<TextEdit>> edits = m_Service.Rename(path, line, col, params["newName"].String(), why);
			if (edits.empty())
			{
				error = why;
				errorCode = -32803; // RequestFailed
			}
			else
			{
				Json changes = Json::Object();
				for (const auto& [file, list] : edits)
				{
					Json items = Json::Array();
					for (const TextEdit& edit : list)
						items.Push(Json::Object().Set("range", RangeOf(file, edit.At)).Set("newText", edit.NewText));
					changes.Set(UriFromPath(file), std::move(items));
				}
				result = Json::Object().Set("changes", std::move(changes));
			}
		}
		else if (method == "textDocument/documentSymbol")
		{
			result = Json::Array();
			std::function<Json(const DocumentSymbol&)> symbolOf = [&](const DocumentSymbol& symbol) {
				Json entry = Json::Object().Set("name", symbol.Name).Set("kind", SymbolKindOf(symbol.Kind));
				if (!symbol.Detail.empty())
					entry.Set("detail", symbol.Detail);
				entry.Set("range", RangeOf(path, symbol.Extent)).Set("selectionRange", RangeOf(path, symbol.At));
				Json children = Json::Array();
				for (const DocumentSymbol& child : symbol.Children)
					children.Push(symbolOf(child));
				entry.Set("children", std::move(children));
				return entry;
			};
			for (const DocumentSymbol& symbol : m_Service.DocumentSymbols(path))
				result.Push(symbolOf(symbol));
		}
		else if (method == "workspace/symbol")
		{
			result = Json::Array();
			for (const WorkspaceSymbol& symbol : m_Service.WorkspaceSymbols(params["query"].String()))
			{
				Json entry = Json::Object().Set("name", symbol.Name).Set("kind", SymbolKindOf(symbol.Kind));
				if (!symbol.Container.empty())
					entry.Set("containerName", symbol.Container);
				entry.Set("location", Json::Object().Set("uri", UriFromPath(symbol.At.Path)).Set("range", RangeOf(symbol.At.Path, symbol.At.At)));
				result.Push(std::move(entry));
			}
		}
		else if (method == "textDocument/inlayHint")
		{
			result = Json::Array();
			const int from = params["range"]["start"]["line"].Int();
			const int to = params["range"]["end"]["line"].Int(from);
			for (const InlayHint& hint : m_Service.InlayHints(path, from, to))
				result.Push(Json::Object().Set("position", Position(path, hint.At)).Set("label", hint.Label).Set("kind", 1));
		}
		else if (method == "textDocument/formatting")
		{
			const Json& options = params["options"];
			const std::string indent = options["insertSpaces"].Bool() ? std::string((size_t)std::max(1, options["tabSize"].Int(4)), ' ') : "\t";
			result = Json::Array();
			for (const TextEdit& edit : m_Service.Format(path, indent))
				result.Push(Json::Object()
						.Set("range", Json::Object()
								.Set("start", Json::Object().Set("line", edit.At.Begin.Line).Set("character", 0))
								.Set("end", Json::Object().Set("line", edit.At.End.Line).Set("character", 0)))
						.Set("newText", edit.NewText));
		}
		else if (isRequest)
		{
			error = "unsupported method '" + method + "'";
			errorCode = -32601; // MethodNotFound
		}

		if (!isRequest)
			return out;
		Json response = Json::Object().Set("jsonrpc", "2.0").Set("id", request["id"]);
		if (errorCode != 0)
			response.Set("error", Json::Object().Set("code", errorCode).Set("message", error));
		else
			response.Set("result", std::move(result));
		// The response goes before the notifications the request raised.
		out.insert(out.begin(), response.Dump());
		return out;
	}

	int LspServer::Serve(std::FILE* in, std::FILE* out)
	{
#ifdef _WIN32
		_setmode(_fileno(in), _O_BINARY);
		_setmode(_fileno(out), _O_BINARY);
#endif
		for (;;)
		{
			// Headers, then an empty line; Content-Length is the body's size.
			long length = -1;
			std::string header;
			bool ended = false;
			for (;;)
			{
				const int c = std::fgetc(in);
				if (c == EOF)
				{
					ended = true;
					break;
				}
				if (c == '\n')
				{
					if (!header.empty() && header.back() == '\r')
						header.pop_back();
					if (header.empty())
						break;
					constexpr const char* kLength = "content-length:";
					std::string lower = header;
					std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
					if (lower.rfind(kLength, 0) == 0)
						length = std::strtol(header.c_str() + std::strlen(kLength), nullptr, 10);
					header.clear();
				}
				else
					header += (char)c;
			}
			if (ended)
				return m_ShutDown ? 0 : 1;
			if (length < 0)
				continue;
			std::string body((size_t)length, '\0');
			if (length > 0 && std::fread(body.data(), 1, (size_t)length, in) != (size_t)length)
				return 1;
			for (const std::string& message : Handle(body))
			{
				std::fprintf(out, "Content-Length: %zu\r\n\r\n", message.size());
				std::fwrite(message.data(), 1, message.size(), out);
			}
			std::fflush(out);
			if (m_Exit)
				return ExitCode();
		}
	}

} // namespace cluau::runtime::detail

namespace cluau::runtime {

	int ServeLanguageServer(const CheckOptions& options)
	{
		detail::LspServer server(options);
		return server.Serve(stdin, stdout);
	}

} // namespace cluau::runtime
