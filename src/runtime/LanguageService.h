#pragma once

// The editor's view of scripts, behind `cluau lsp`: the checker's analysis
// (Analysis.h) over the files an editor has open, answering what is wrong,
// what may come next, what a name is and where it was declared. Paths are
// files' paths; positions are 0-based lines and byte columns in the text as
// written (a .cluau file's, mapped through its translation both ways).

#include "Analysis.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Luau {
	struct Position;
	struct Location;
}

namespace cluau::runtime::detail {

	enum class CompletionKind { Variable, Function, Method, Property, Keyword, Type, Module, Value };

	struct CompletionItem
	{
		std::string Label;
		CompletionKind Kind = CompletionKind::Variable;
		std::string Detail;     // the type, or what the item is
		std::string InsertText; // a snippet ("$0" is the caret); "" = the label
		bool Deprecated = false;
	};

	struct CompletionList
	{
		int ReplaceFrom = 0; // the column the typed word starts at
		std::vector<CompletionItem> Items;
	};

	struct HoverInfo
	{
		std::string Text;          // `name: type`
		std::string Documentation; // the comment above a declaration
		Range At;
	};

	struct SignatureInfo
	{
		std::string Label;                       // `name(a: number, b: string): boolean`
		std::vector<std::pair<int, int>> Params; // byte ranges in Label
		int Active = -1;                         // the argument the caret is in
	};

	struct Location
	{
		std::string Path;
		Range At;
	};

	struct TextEdit
	{
		Range At;
		std::string NewText;
	};

	enum class SymbolKind { Class, Field, Method, Function, Variable, Enum };

	struct DocumentSymbol
	{
		std::string Name;
		SymbolKind Kind = SymbolKind::Variable;
		std::string Detail;
		Range At;     // the name
		Range Extent; // the whole declaration
		std::vector<DocumentSymbol> Children;
	};

	class LanguageService
	{
	public:
		explicit LanguageService(const CheckOptions& options = {});

		// An editor's buffers: opened, changed (the whole text) and closed.
		void Open(const std::string& path, std::string text);
		void Change(const std::string& path, std::string text);
		void Close(const std::string& path);
		// A file changed on disk (saved, or changed by another program).
		void Changed(const std::string& path);
		bool IsOpen(const std::string& path) const;
		std::vector<std::string> OpenFiles() const;

		// Each open file's findings (each is checked again where it or a
		// module it requires changed).
		std::map<std::string, std::vector<CheckFinding>> Diagnostics();

		CompletionList Complete(const std::string& path, int line, int col);
		std::optional<HoverInfo> Hover(const std::string& path, int line, int col);
		std::optional<SignatureInfo> Signature(const std::string& path, int line, int col);
		std::optional<Location> Definition(const std::string& path, int line, int col);
		// A local's uses in its file (its declaration first).
		std::vector<Range> References(const std::string& path, int line, int col);
		// The edits renaming the local at the position; `error` says why
		// there are none.
		std::vector<TextEdit> Rename(const std::string& path, int line, int col, const std::string& newName, std::string& error);
		std::vector<DocumentSymbol> DocumentSymbols(const std::string& path);
		// The edit that formats the file as `cluau fmt` would (none when it
		// is formatted already, or cannot be read).
		std::vector<TextEdit> Format(const std::string& path, const std::string& indent);

		// The text the service holds for a file (its buffer, else the disk).
		std::optional<std::string> TextOf(const std::string& path) const;

	private:
		std::string Name(const std::string& path) const;
		Luau::Position In(const std::string& name, int line, int col) const;
		Range Out(const std::string& name, const Luau::Location& at) const;
		bool Prepare(const std::string& name);

		Analysis m_Analysis;
		std::map<std::string, std::string> m_Open; // module name -> the path the editor gave
	};

} // namespace cluau::runtime::detail
