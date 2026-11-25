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
	class AstExprIndexName;
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

	struct WorkspaceSymbol
	{
		std::string Name;
		SymbolKind Kind = SymbolKind::Variable;
		std::string Container; // the class a member belongs to
		Location At;
	};

	struct InlayHint
	{
		Position At;       // where it shows (after a name)
		std::string Label; // ": number"
	};

	// A diagnostic the editor asks code actions for.
	struct DiagnosticRef
	{
		Range At;
		std::string Code;
		std::string Message;
	};

	// A quick fix: its title and edits by file.
	struct CodeAction
	{
		std::string Title;
		std::map<std::string, std::vector<TextEdit>> Edits;
		bool Preferred = false;
	};

	enum class TokenType { Namespace, Class, Enum, Type, Parameter, Variable, Property, Function, Method };
	enum TokenModifier : unsigned { Declaration = 1, Readonly = 2, DefaultLibrary = 4 };

	// A name's kind, for highlighting (what the checker knows of it).
	struct SemanticToken
	{
		Position At; // 0-based line and byte column
		int Length = 0;
		TokenType Type = TokenType::Variable;
		unsigned Modifiers = 0;
	};

	// A function, in the call hierarchy.
	struct CallItem
	{
		std::string Name;
		SymbolKind Kind = SymbolKind::Function;
		std::string Detail; // the file, or the class
		Location At;        // its name where it is declared
	};

	struct Call
	{
		CallItem Item;            // the caller (incoming) or the callee (outgoing)
		std::vector<Range> Sites; // the calls: in the caller's file
	};

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
		// The editor's workspace folders: where references, renames and
		// symbols look beyond the files that are open.
		void SetWorkspace(std::vector<std::string> folders);

		// The uses of the name at the position, its declaration first: a
		// local's (or a script's top-level function's) in its file; a class
		// member's or a module's exported key's in every file of the
		// workspace that uses it.
		std::vector<Location> References(const std::string& path, int line, int col);
		// The edits renaming the name at the position, by file; `error`
		// says why there are none.
		std::map<std::string, std::vector<TextEdit>> Rename(const std::string& path, int line, int col, const std::string& newName, std::string& error);
		std::vector<DocumentSymbol> DocumentSymbols(const std::string& path);
		// The classes, members, functions and variables of the workspace's
		// files whose names hold the query's letters in order (any case).
		std::vector<WorkspaceSymbol> WorkspaceSymbols(const std::string& query);
		// The types of the names `let`, `const` and `for` declare without
		// one, on lines `fromLine` to `toLine`.
		std::vector<InlayHint> InlayHints(const std::string& path, int fromLine, int toLine);

		// The quick fixes for those of the file's diagnostics given: a
		// misspelt member or global's likely names, a standard module's
		// require, the line's `any` exemption.
		std::vector<CodeAction> CodeActions(const std::string& path, const std::vector<DiagnosticRef>& diagnostics);
		// Every name in the file with its kind, in order.
		std::vector<SemanticToken> SemanticTokens(const std::string& path);
		// The function at the position (named or called there), and what
		// calls it or what it calls.
		std::optional<CallItem> PrepareCall(const std::string& path, int line, int col);
		std::vector<Call> IncomingCalls(const CallItem& item);
		std::vector<Call> OutgoingCalls(const CallItem& item);
		// The edit that formats the file as `cluau fmt` would (none when it
		// is formatted already, or cannot be read).
		std::vector<TextEdit> Format(const std::string& path, const std::string& indent);

		// The text the service holds for a file (its buffer, else the disk).
		std::optional<std::string> TextOf(const std::string& path) const;

	private:
		std::string Name(const std::string& path) const;
		std::string PathOf(const std::string& name) const;
		std::optional<Location> Find(const std::string& path, int line, int col);
		// A checked location in a module, back in its text where the text
		// holds `symbol` there (or where a class declares the member).
		std::optional<Location> Mapped(const std::string& module, const Luau::Location& at, const std::string& symbol) const;
		// Where the member `index` names is declared.
		std::optional<Location> MemberDeclaration(const std::string& module, const Luau::AstExprIndexName* index);
		// References; `declaredIn` is the module declaring a member ("" for a local).
		std::vector<Location> ReferencesOf(const std::string& path, int line, int col, std::string& declaredIn);
		// A function's item from where it is declared (nullopt when what is
		// declared there is not a function).
		std::optional<CallItem> CallItemAt(const Location& declaration);
		// The item of the function the module's position is inside ("(top
		// level)" outside any).
		CallItem Enclosing(const std::string& module, const std::string& file, const Luau::Position& at);
		// The workspace's script files, and the open ones.
		std::vector<std::string> SearchFiles() const;
		Luau::Position In(const std::string& name, int line, int col) const;
		Range Out(const std::string& name, const Luau::Location& at) const;
		bool Prepare(const std::string& name);

		Analysis m_Analysis;
		std::map<std::string, std::string> m_Open; // module name -> the path the editor gave
		std::vector<std::string> m_Workspace;
	};

} // namespace cluau::runtime::detail
