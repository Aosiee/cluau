#pragma once

// cluau - C-syntax-flavoured Luau. Translate() turns a .cluau source into
// standard Luau text. Every output line is the same line of the input (only
// columns move), and the SourceMap converts positions both ways. The
// translator has no dependencies and knows nothing about any host: what a
// class's attributes mean and which members `self` starts with come from
// the host's ClassProfile.
//
// Lines and columns are 0-based; columns count bytes.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cluau {

	struct Position
	{
		int Line = 0;
		int Col = 0;
		bool operator==(const Position& other) const { return Line == other.Line && Col == other.Col; }
		bool operator<(const Position& other) const { return Line != other.Line ? Line < other.Line : Col < other.Col; }
	};

	struct Range
	{
		Position Begin;
		Position End;
	};

	struct Diagnostic
	{
		Range At;
		std::string Message;
		bool Warning = false;
	};

	// Column anchors per line: a pair (source column, Luau column) at every
	// token boundary the translation touched. Between anchors the text is
	// copied byte for byte.
	class SourceMap
	{
	public:
		// A source position in the Luau text (the same line).
		Position ToLuau(Position source) const;
		// A Luau position in the source (the same line). A position inside
		// text the translator generated maps to the start of what it stands
		// for.
		Position FromLuau(Position luau) const;

		void AddAnchor(int line, int sourceCol, int luauCol);

	private:
		struct Anchor
		{
			int Source = 0;
			int Luau = 0;
		};
		std::map<int, std::vector<Anchor>> m_Lines;
	};

	// An `// allow-any: <reason>` comment (the strict rules' line exemption).
	struct Exemption
	{
		int Line = 0;
		std::string Reason;
	};

	struct ClassField
	{
		std::string Name;
		std::string Type;      // the annotation's text ("" when absent)
		std::string Default;   // the default expression's text, translated ("" when absent)
		std::vector<std::string> Attributes; // "export", "replicated", ... (without '@')
		Range At;              // the field's name
	};

	struct ClassMethod
	{
		std::string Name;
		std::string Params;     // the parameter list's text, translated, without the parentheses
		std::string ReturnType; // "" when absent
		bool ReturnsValue = false; // the body has a `return <expression>`
		Range At;               // the method's name
		int BodyBeginLine = 0;
		int BodyEndLine = 0;
	};

	// `signal Name(param: Type, ...)` in a class body (CLUAU_PLAN §5 Q13): a
	// signal the class emits, with its payload's types. A host types its
	// emit / connect calls from these; the module table lists the names.
	struct ClassSignal
	{
		std::string Name;
		std::string Params; // the parameter list's text, without the parentheses
		Range At;           // the signal's name
	};

	struct ClassInfo
	{
		std::string Name;
		Range At;            // the class's name
		int BodyBeginLine = 0;
		int BodyEndLine = 0; // the closing brace's line
		std::vector<ClassField> Fields;
		std::vector<ClassMethod> Methods;
		std::vector<std::string> Statics; // `static Name = value` members (of the module table)
		std::vector<ClassSignal> Signals;
	};

	// What a class lowers to beyond cluau's generic shape (a module table, a
	// type, methods). Every string may use "$Class" for the class's name.
	struct ClassProfile
	{
		// Emitted right after `local $Class = ` on the class line.
		std::string TableInit = "{ Fields = {} }";
		// Members every instance has before its own fields (each ends with
		// ", "), e.g. "Entity: Entity, ".
		std::string BaseMembers;
		// The statement a field lowers to (on the field's line). Arguments:
		// the field and the class name. Returning "" drops the field from
		// the module table (it is still part of the type). The default
		// lowering is "$Class.Fields.<Name> = <Default>".
		std::function<std::string(const ClassField&, const std::string& className, std::string& error)> LowerField;
		// A field's Luau type in the class type (null = its declared type),
		// e.g. a host type spelled differently in Luau.
		std::function<std::string(const ClassField&)> FieldType;
		// A signal's member type in the class type (null = signals are not
		// members), e.g. "Signal<vec3>" - a host whose instances carry their
		// signals as objects (`self.Collected:Emit(...)`).
		std::function<std::string(const ClassSignal&)> SignalType;
		// The module-table member the signal names are listed in (`signal`
		// declarations lower to `$Class.<SignalsMember> = { "A", ... }` on
		// the class line); "" = signals are not allowed.
		std::string SignalsMember = "Signals";
	};

	struct TranslateOptions
	{
		const ClassProfile* Profile = nullptr; // nullptr = the generic profile
		bool Manifest = false;                 // reject function / while / do-while / class
	};

	struct TranslateResult
	{
		std::string Luau;
		SourceMap Map;
		std::vector<Diagnostic> Diagnostics;
		std::vector<Exemption> Exemptions;
		std::vector<Range> ConstNames;      // each `const` local's name (source positions)
		std::vector<Range> Generated;       // Luau-text ranges the translator generated (type aliases, ...)
		std::optional<ClassInfo> Class;
		bool HasErrors() const;
	};

	TranslateResult Translate(std::string_view source, const TranslateOptions& options = {});

	// "1.2.3": enters a host's cache keys / build digests, so a translator
	// change re-translates.
	const char* Version();

	inline bool IsCluauPath(std::string_view path)
	{
		return path.size() >= 6 && path.substr(path.size() - 6) == ".cluau";
	}

} // namespace cluau
