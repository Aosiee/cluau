#include "test.h"

#include "cluau/cluau.h"
#include "cluau/format.h"

#include <algorithm>

namespace {

	std::string Luau(const std::string& source, const cluau::TranslateOptions& options = {})
	{
		return cluau::Translate(source, options).Luau;
	}

	int Lines(const std::string& s) { return (int)std::count(s.begin(), s.end(), '\n'); }

	bool HasError(const cluau::TranslateResult& r, const std::string& needle)
	{
		return std::any_of(r.Diagnostics.begin(), r.Diagnostics.end(),
			[&](const cluau::Diagnostic& d) { return d.Message.find(needle) != std::string::npos; });
	}

} // namespace

TEST("statements: if chains lower to elseif and one end")
{
	CHECK_EQ(Luau("if (a) { x() } else if (b) { y() } else { z() }\n"),
		std::string("if (a) then x()  elseif  (b) then y()  else  z() end\n"));
	CHECK_EQ(Luau("while (n > 0) { n-- }"), std::string("while (n > 0) do n -= 1 end"));
	CHECK_EQ(Luau("for (i = 1, 10) { f(i) }"), std::string("for  i = 1, 10  do f(i) end"));
	CHECK_EQ(Luau("for (let k, v in t) { f(k) }"), std::string("for   k, v in t  do f(k) end"));
	CHECK_EQ(Luau("do { n++ } while (n < 3);"), std::string("repeat  n += 1 until not (n < 3);"));
	CHECK_EQ(Luau("x = 1\n{ let y = 2 }"), std::string("x = 1\ndo local y = 2 end"));
}

TEST("functions: statements, locals, literals, return types with table types")
{
	CHECK_EQ(Luau("function f(a: number): number { return a }"), std::string("function f(a: number): number  return a end"));
	CHECK_EQ(Luau("local function g() { }"), std::string("local function g()  end"));
	CHECK_EQ(Luau("let h = function(x) { return x }"), std::string("local h = function(x)  return x end"));
	CHECK_EQ(Luau("function t(): {number} { return {1} }"), std::string("function t(): {number}  return {1} end"));
	CHECK_EQ(Luau("function M:Run<T>(x: T): (T, string) { return x, \"\" }"),
		std::string("function M:Run<T>(x: T): (T, string)  return x, \"\" end"));
	CHECK_EQ(Luau("function all<R...>(f: () -> R...): R... { return f() }"), std::string("function all<R...>(f: () -> R...): R...  return f() end"));
}

TEST("tables stay tables: constructors, call sugar, nested functions")
{
	CHECK_EQ(Luau("let t = { a = 1, f = function() { return 2 } }"),
		std::string("local t = { a = 1, f = function()  return 2 end }"));
	CHECK_EQ(Luau("plugin \"audio\" { enabled = true }"), std::string("plugin \"audio\" { enabled = true }"));
	CHECK_EQ(Luau("f({ x = 1 })"), std::string("f({ x = 1 })"));
	CHECK_EQ(Luau("let t =\n{\n\tx = 1,\n}"), std::string("local t =\n{\n\tx = 1,\n}"));
}

TEST("operators and comments")
{
	CHECK_EQ(Luau("if (a != b && !c || d) { }"), std::string("if (a ~= b  and  not c  or  d) then end"));
	CHECK_EQ(Luau("q = a ~/ b"), std::string("q = a // b"));
	CHECK_EQ(Luau("q ~/= 2"), std::string("q //= 2"));
	CHECK_EQ(Luau("x++ // done"), std::string("x += 1 -- done"));
	CHECK_EQ(Luau("/* a\nb */ y = 1"), std::string("--[[ a\nb ]] y = 1"));
	CHECK_EQ(Luau("/* ]] */"), std::string("--[=[ ]] ]=]"));
	CHECK_EQ(Luau("s = `{a != b}x`"), std::string("s = `{a ~= b}x`"));
	CHECK_EQ(Luau("const x = 1"), std::string("local x = 1"));
}

TEST("lines are preserved, and the map converts columns both ways")
{
	const std::string src =
		"if (a && b) {\n"
		"\tlet value = a ~/ 2\n"
		"}\n";
	const cluau::TranslateResult r = cluau::Translate(src);
	CHECK_EQ(Lines(r.Luau), Lines(src));
	// `value` on line 1: "\tlet value" -> "\tlocal value".
	CHECK_EQ(r.Map.ToLuau({ 1, 5 }).Col, 7);
	CHECK_EQ(r.Map.FromLuau({ 1, 7 }).Col, 5);
	// `b` on line 0 sits after `&&` (2 chars) which became " and " (5).
	CHECK_EQ(r.Map.ToLuau({ 0, 9 }).Col, 12);
	CHECK_EQ(r.Map.FromLuau({ 0, 12 }).Col, 9);
}

TEST("classes: header, fields, methods, the return")
{
	const std::string src =
		"class Player {\n"
		"\t@export Speed: number = 4\n"
		"\tVelocityY: number = 0\n"
		"\tfunction OnStart() {\n"
		"\t\tself.VelocityY = self.Speed\n"
		"\t}\n"
		"\tfunction Step(dt: number): number {\n"
		"\t\treturn dt\n"
		"\t}\n"
		"}\n";
	const cluau::TranslateResult r = cluau::Translate(src);
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), Lines(src));
	CHECK(r.Class.has_value());
	CHECK_EQ(r.Class->Name, std::string("Player"));
	CHECK_EQ(r.Class->Fields.size(), (size_t)2);
	CHECK_EQ(r.Class->Methods.size(), (size_t)2);
	CHECK(r.Luau.find("local Player = { Fields = {} }; export type Player = { Speed: number, VelocityY: number, "
		"OnStart: (self: Player) -> (), Step: (self: Player, dt: number) -> number, }") != std::string::npos);
	CHECK(r.Luau.find("\tPlayer.Fields.Speed = 4") != std::string::npos);
	CHECK(r.Luau.find("\tfunction Player.OnStart(self: Player) \n") != std::string::npos);
	CHECK(r.Luau.find("function Player.Step(self: Player, dt: number): number ") != std::string::npos);
	CHECK(r.Luau.find("\nreturn Player\n") != std::string::npos);
	CHECK_EQ(r.Generated.size(), (size_t)1);
}

TEST("classes: a host profile lowers fields")
{
	cluau::ClassProfile profile;
	profile.TableInit = "{ Properties = {}, Fields = {} }";
	profile.BaseMembers = "Entity: Entity, ";
	profile.LowerField = [](const cluau::ClassField& f, const std::string& cls, std::string&) -> std::string {
		const bool exported = std::find(f.Attributes.begin(), f.Attributes.end(), "export") != f.Attributes.end();
		return exported ? cls + ".Properties." + f.Name + " = { type = \"" + f.Type + "\", default = " + f.Default + " }"
						: cls + ".Fields." + f.Name + " = " + f.Default;
	};
	cluau::TranslateOptions options;
	options.Profile = &profile;
	const std::string luau = Luau("class C {\n\t@export Speed: number = 4\n\tY: number = 1 ~/ 2\n}\n", options);
	CHECK(luau.find("export type C = { Entity: Entity, Speed: number, Y: number, }") != std::string::npos);
	CHECK(luau.find("C.Properties.Speed = { type = \"number\", default = 4 }") != std::string::npos);
	CHECK(luau.find("C.Fields.Y = 1 // 2") != std::string::npos);
}

TEST("diagnostics: Luau block words, unbalanced braces, missing parentheses, exemptions")
{
	CHECK(HasError(cluau::Translate("if x then end"), "cluau uses braces"));
	CHECK(HasError(cluau::Translate("if x > 0 { }"), "parenthesised"));
	const cluau::TranslateResult open = cluau::Translate("function f() {\n\tif (a) {\n");
	CHECK(HasError(open, "no closing"));
	CHECK(open.Luau.find("end end") != std::string::npos); // closed at the end, still usable
	const cluau::TranslateResult ex = cluau::Translate("let a = f() // allow-any: the codec returns any\nlet b = g() // allow-any:\n");
	CHECK_EQ(ex.Exemptions.size(), (size_t)1);
	CHECK_EQ(ex.Exemptions[0].Line, 0);
	CHECK(HasError(ex, "needs a reason"));
	cluau::TranslateOptions manifest;
	manifest.Manifest = true;
	CHECK(HasError(cluau::Translate("function f() { }", manifest), "manifests are declarations"));
	CHECK(!cluau::Translate("plugin \"a\" { deps = { \"b\" } }", manifest).HasErrors());
}

TEST("const names are recorded")
{
	const cluau::TranslateResult r = cluau::Translate("const a, b: number = 1, 2");
	CHECK_EQ(r.ConstNames.size(), (size_t)2);
	CHECK_EQ(r.ConstNames[1].Begin.Col, 9);
}

TEST("classes: static members are module-table members, translated as code")
{
	const std::string src =
		"class Keeper {\n"
		"\tstatic Signals = { \"Collect\" }\n"
		"\tstatic Server = {\n"
		"\t\tReset = function(self: Keeper, caller: Entity?) {\n"
		"\t\t\tif (caller != nil) { self.Score = 0 }\n"
		"\t\t},\n"
		"\t}\n"
		"\tScore: number = 0\n"
		"}\n";
	const cluau::TranslateResult r = cluau::Translate(src);
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), Lines(src));
	CHECK(r.Luau.find("Keeper.Signals = { \"Collect\" }\n") != std::string::npos);
	CHECK(r.Luau.find("Keeper.Server = {\n") != std::string::npos);
	CHECK(r.Luau.find("Reset = function(self: Keeper, caller: Entity?) \n") != std::string::npos);
	CHECK(r.Luau.find("if (caller ~= nil) then self.Score = 0 end") != std::string::npos);
	CHECK(r.Luau.find("\t\tend,\n") != std::string::npos);
	CHECK(r.Luau.find("Keeper.Fields.Score = 0") != std::string::npos);
	CHECK_EQ(r.Class->Statics.size(), (size_t)2);
	CHECK(r.Luau.find("export type Keeper = { Score: number, }") != std::string::npos);
}

TEST("mode comments are refused")
{
	const cluau::TranslateResult r = cluau::Translate("//!nonstrict\nlet x = 1\n");
	CHECK(HasError(r, "mode comments"));
	CHECK(r.Luau.rfind("-- !nonstrict", 0) == 0);
}

TEST("if-expressions: if (c) a else b where an expression is expected")
{
	CHECK_EQ(Luau("let s = if (b) 2 else 1"), std::string("local s = if (b) then 2 else 1"));
	CHECK_EQ(Luau("let l = if (n == 0) \"none\" else if (n == 1) \"one\" else \"many\""),
		std::string("local l = if (n == 0) then \"none\" elseif  (n == 1) then \"one\" else \"many\""));
	CHECK_EQ(Luau("return if (a && !b) x else y"), std::string("return if (a  and  not b) then x else y"));
	// A `{` after the condition or the else is a table, not a block.
	CHECK_EQ(Luau("f(if (a) { 1 } else {}, 2)"), std::string("f(if (a) then { 1 } else {}, 2)"));
	CHECK_EQ(Luau("x = if (a) if (b) 1 else 2 else 3"), std::string("x = if (a) then if (b) then 1 else 2 else 3"));
	CHECK_EQ(Luau("x = { k = if (a) 1 else 2 }"), std::string("x = { k = if (a) then 1 else 2 }"));
	// A statement if after an if-expression, and one inside a branch's function.
	CHECK_EQ(Luau("x = if (a) 1 else 2\nif (x) { y() } else { z() }\n"),
		std::string("x = if (a) then 1 else 2\nif (x) then y()  else  z() end\n"));
	const cluau::TranslateResult inner = cluau::Translate("g = if (a) function() { if (c) { p() } else { q() } } else nil\n");
	CHECK(!inner.HasErrors());
	CHECK_EQ(inner.Luau, std::string("g = if (a) then function()  if (c) then p()  else  q() end end else nil\n"));
	// Lines stay put across a multi-line if-expression.
	const std::string multi = "let v = if (a)\n\t1\nelse if (b)\n\t2\nelse\n\t3\n";
	CHECK_EQ(Luau(multi), std::string("local v = if (a) then\n\t1\nelseif  (b) then\n\t2\nelse\n\t3\n"));
	// Field defaults and interpolation segments translate theirs too.
	CHECK(Luau("class C {\n\tY: number = if (a) 1 else 2\n}\n").find("C.Fields.Y = if (a) then 1 else 2") != std::string::npos);
	CHECK_EQ(Luau("s = `{if (a) \"x\" else \"y\"}`"), std::string("s = `{if (a) then \"x\" else \"y\"}`"));
}

TEST("if-expressions: a missing else or parenthesis is reported")
{
	CHECK(HasError(cluau::Translate("let x = if (a) 1\n"), "needs an else"));
	CHECK(HasError(cluau::Translate("f(if (a) 1)"), "needs an else"));
	CHECK(HasError(cluau::Translate("let x = if (a) 1\nif (b) { } else { }\n"), "needs an else"));
	CHECK(HasError(cluau::Translate("let x = if a 1 else 2"), "parenthesised"));
	CHECK(!cluau::Translate("let x = if (a) 1 else 2\n").HasErrors());
}

TEST("classes: signal declarations list their names on the class line, keeping every line")
{
	const std::string src =
		"class Coin {\n"
		"\tsignal Collect()\n"
		"\tsignal Collected(position: vec3, by: Entity)\n"
		"\tSpeed: number = 1\n"
		"}\n";
	const cluau::TranslateResult r = cluau::Translate(src);
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), Lines(src));
	CHECK(r.Class.has_value() && r.Class->Signals.size() == (size_t)2);
	if (!r.Class || r.Class->Signals.size() != 2)
		return;
	CHECK_EQ(r.Class->Signals[0].Name, std::string("Collect"));
	CHECK_EQ(r.Class->Signals[0].Params, std::string(""));
	CHECK_EQ(r.Class->Signals[1].Params, std::string("position: vec3, by: Entity"));
	CHECK_EQ(r.Class->Signals[1].At.Begin.Line, 2);
	CHECK(r.Luau.find("; Coin.Signals = { \"Collect\", \"Collected\" }") != std::string::npos);
	CHECK(r.Luau.find("signal") == std::string::npos);
	CHECK(HasError(cluau::Translate("class C {\n\tsignal Hit(amount)\n}\n"), "declare their types"));
	CHECK(HasError(cluau::Translate("class C {\n\tsignal A()\n\tsignal A()\n}\n"), "declared twice"));
	CHECK(HasError(cluau::Translate("class C {\n\tstatic Signals = { \"A\" }\n\tsignal A()\n}\n"), "not `static Signals`"));
	// A field named `signal` stays a field.
	const cluau::TranslateResult field = cluau::Translate("class C {\n\tsignal: number = 1\n}\n");
	CHECK(!field.HasErrors());
	CHECK(field.Class->Fields.size() == (size_t)1);
}

TEST("classes: a profile's FieldType spells a field's Luau type")
{
	cluau::ClassProfile profile;
	profile.FieldType = [](const cluau::ClassField& f) -> std::string {
		return f.Type.rfind("AssetRef<", 0) == 0 ? std::string("string") : f.Type;
	};
	cluau::TranslateOptions options;
	options.Profile = &profile;
	const std::string luau = Luau("class C {\n\tPrefab: AssetRef<Scene> = \"a/b.tasset\"\n\tN: number = 1\n}\n", options);
	CHECK(luau.find("export type C = { Prefab: string, N: number, }") != std::string::npos);
}

TEST("classes: a profile's SignalType makes each signal a member of the class type")
{
	cluau::ClassProfile profile;
	profile.SignalType = [](const cluau::ClassSignal& s) -> std::string { return "Signal<(" + s.Params + ")>"; };
	cluau::TranslateOptions options;
	options.Profile = &profile;
	const std::string luau = Luau("class C {\n\tsignal Hit(by: number)\n\tN: number = 1\n}\n", options);
	CHECK(luau.find("export type C = { N: number, Hit: Signal<(by: number)>, }") != std::string::npos);
	CHECK(luau.find("C.Signals = { \"Hit\" }") != std::string::npos);
	// Without the hook, signals stay out of the type.
	CHECK(Luau("class C {\n\tsignal Hit(by: number)\n}\n").find("Hit: ") == std::string::npos);
}

TEST("scripts: a #! first line is a comment, on its line")
{
	CHECK_EQ(Luau("#!/usr/bin/env cluau\nlet x = 1"), std::string("--#!/usr/bin/env cluau\nlocal x = 1"));
	// Only the first line: `#` elsewhere is the length operator.
	CHECK_EQ(Luau("let n = #t"), std::string("local n = #t"));
}

TEST("scripts: script mode allows several classes, each a local the code after it uses")
{
	cluau::TranslateOptions options;
	options.Script = true;
	const cluau::TranslateResult r = cluau::Translate("class A {\n\tV: number = 1\n}\nclass B {\n}\nlet a = A\n", options);
	CHECK(!r.HasErrors());
	CHECK_EQ(r.Classes.size(), (size_t)2);
	CHECK_EQ(r.Class->Name, std::string("A"));
	CHECK(r.Luau.find("return") == std::string::npos);
	CHECK_EQ(Lines(r.Luau), 6);
	// A file whose last statement is a class still returns it (a module).
	CHECK(Luau("class A {\n}\nclass B {\n};\n", options).find("return B;") != std::string::npos);
	// Outside script mode, one class, last.
	CHECK(HasError(cluau::Translate("class A {\n}\nclass B {\n}\n"), "at most one class"));
}

TEST("scripts: the script profile constructs instances and evaluates each default afresh")
{
	cluau::TranslateOptions options;
	options.Script = true;
	options.Profile = &cluau::ScriptProfile();
	const std::string luau = Luau("class P {\n\tTags: {string} = {}\n\tN: number\n}\n", options);
	CHECK(luau.rfind("local P = setmetatable({ Fields = {}, __name = \"P\", __instance = (nil :: any) :: P }, __cluau_meta :: { __call: (unknown) -> P, __iter: (unknown) -> ((unknown, string?) -> (string?, unknown), unknown) }); ", 0) == 0);
	CHECK(luau.find("export type P = typeof(setmetatable({} :: { Tags: {string}, N: number, }, {} :: { __index: typeof(P), }))") != std::string::npos);
	// Init's parameters type the constructor; metamethods are the instances' metatable's.
	const std::string vec = Luau("class V {\n\tfunction Init(x: number) { }\n\tfunction __add(o: V): V { return o }\n\tfunction Len(): number { return 1 }\n}\n", options);
	CHECK(vec.find("__call: (unknown, x: number) -> V, __iter: ") != std::string::npos);
	CHECK(vec.find("{} :: { __index: typeof(V), __add: (self: V, o: V) -> V, }") != std::string::npos);
	CHECK_EQ(Lines(vec), 5);
	CHECK(luau.find("P.Fields.Tags = function() return {} end") != std::string::npos);
	CHECK(luau.find("P.Fields.N") == std::string::npos);
}

TEST("scripts: REPL mode makes top-level declarations globals")
{
	cluau::TranslateOptions options;
	options.Repl = true;
	CHECK_EQ(Luau("let x: number = 1", options), std::string(" x  = 1"));
	CHECK_EQ(Luau("const a, b: {number}", options), std::string(" a, b = nil "));
	CHECK_EQ(Luau("local function f() { let y = 2 }", options), std::string(" function f()  local y = 2 end"));
	CHECK_EQ(Luau("if (c) { let z = 3 }", options), std::string("if (c) then local z = 3 end"));
	CHECK(Luau("class P {\n}", options).rfind(" P = ", 0) == 0);
}

TEST("statements: an empty block and a block's end beside a word stay apart")
{
	CHECK_EQ(Luau("while (true) {}"), std::string("while (true) do end"));
	CHECK_EQ(Luau("function f() {}"), std::string("function f() end"));
	CHECK_EQ(Luau("if (a) {}else{}"), std::string("if (a) then else end"));
	CHECK_EQ(Luau("{ x() }print(1)"), std::string("do x() end print(1)"));
}

TEST("scripts: a class extends another; super calls the base's methods")
{
	cluau::TranslateOptions options;
	options.Script = true;
	options.Profile = &cluau::ScriptProfile();
	const cluau::TranslateResult r = cluau::Translate(
		"class A {\n\tfunction Init(n: number) { }\n}\nclass B extends A {\n\tfunction Hi(): number { return super:Get() + super.Count }\n}\nclass C extends lib.Base {\n}\n", options);
	CHECK(!r.HasErrors());
	CHECK_EQ(r.Classes.size(), (size_t)3);
	CHECK_EQ(r.Classes[1].Base, std::string("A"));
	CHECK_EQ(r.Classes[2].Base, std::string("lib.Base"));
	CHECK_EQ(Lines(r.Luau), 8);
	// B inherits A's constructor; its instances are A's and its own.
	CHECK(r.Luau.find("__base = A, __instance = (nil :: any) :: B }, __cluau_meta :: { __call: (unknown, n: number) -> B, __index: typeof(A), __iter: ") != std::string::npos);
	CHECK(r.Luau.find("export type B = typeof(A.__instance) & typeof(setmetatable(") != std::string::npos);
	CHECK(r.Luau.find("return A.Get(self) + A.Count") != std::string::npos);
	// Another module's base: its constructor's parameters are unknown.
	CHECK(r.Luau.find("__call: (unknown, ...any) -> C, __index: typeof(lib.Base), __iter: ") != std::string::npos);
	// The `extends` clause is gone from the line.
	CHECK(r.Luau.find("extends") == std::string::npos);

	CHECK(HasError(cluau::Translate("class A {\n\tfunction F() { super:F() }\n}\n", options), "super needs a base class"));
	CHECK(HasError(cluau::Translate("class B extends A {\n}\n"), "do not extend"));
	CHECK(HasError(cluau::Translate("class B extends {\n}\n", options), "extends names the base class"));
}

TEST("scripts: a source that ends inside a block, bracket, comment or long string is unfinished")
{
	CHECK(cluau::Translate("if (x) {\n\tprint(1)\n").Unfinished);
	CHECK(cluau::Translate("print(1,\n").Unfinished);
	CHECK(cluau::Translate("let t = {\n").Unfinished);
	CHECK(cluau::Translate("/* a comment\n").Unfinished);
	CHECK(cluau::Translate("let s = [[long\n").Unfinished);
	CHECK(cluau::Translate("let s = `text {x}\n").Unfinished);
	// Finished, or wrong rather than unfinished.
	CHECK(!cluau::Translate("print(1)\n").Unfinished);
	CHECK(!cluau::Translate("let s = \"unterminated\n").Unfinished);
	CHECK(!cluau::Translate("f(1 }\n").Unfinished);
}

namespace {

	std::string Formatted(const std::string& source) { return cluau::Format(source).Text; }

} // namespace

TEST("format: indentation follows the brackets open at each line's start")
{
	CHECK_EQ(Formatted("if (a) {\nprint(1)\n  } else {\n      print(2)\n}\n"),
		std::string("if (a) {\n\tprint(1)\n} else {\n\tprint(2)\n}\n"));
	// Several brackets opened on one line are one level; the closing line is back at the opener's.
	CHECK_EQ(Formatted("print(call(1,\n{\nx = 1,\n}))\n"), std::string("print(call(1,\n\t{\n\t\tx = 1,\n\t}))\n"));
	CHECK_EQ(Formatted("class P {\n  X: number = 0\nfunction F() {\nreturn 1\n}\n}\n"),
		std::string("class P {\n\tX: number = 0\n\tfunction F() {\n\t\treturn 1\n\t}\n}\n"));
}

TEST("format: a continued statement is one level in, once")
{
	CHECK_EQ(Formatted("const x = a +\nb +\nc\n"), std::string("const x = a +\n\tb +\n\tc\n"));
	CHECK_EQ(Formatted("let t = obj\n:Method()\n.Field\n"), std::string("let t = obj\n\t:Method()\n\t.Field\n"));
	CHECK_EQ(Formatted("let v = if (x) 1\nelse 2\n"), std::string("let v = if (x) 1\n\telse 2\n"));
	// A bracket opened on the line before already gives the level.
	CHECK_EQ(Formatted("f(a +\nb)\n"), std::string("f(a +\n\tb)\n"));
	CHECK_EQ(Formatted("f(\na +\nb)\n"), std::string("f(\n\ta +\n\t\tb)\n"));
	// A line starting with `-` may start an expression: not a continuation.
	CHECK_EQ(Formatted("x = 1\n-y()\n"), std::string("x = 1\n-y()\n"));
	CHECK_EQ(Formatted("return 1\n+ 2\n"), std::string("return 1\n\t+ 2\n"));
}

TEST("format: whitespace inside strings and comments that span lines stays")
{
	const std::string source = "let s = [[\n  keep   this   \n\tas is]]   \n/* a\n     comment */\nlet t = 1   \n";
	CHECK_EQ(Formatted(source), std::string("let s = [[\n  keep   this   \n\tas is]]\n/* a\n     comment */\nlet t = 1\n"));
}

TEST("format: trailing blank lines go, line breaks keep their kind, a second pass changes nothing")
{
	CHECK_EQ(Formatted("let a = 1\n\n\n"), std::string("let a = 1\n"));
	CHECK_EQ(Formatted("if (a) {\r\nf()\r\n}"), std::string("if (a) {\r\n\tf()\r\n}\r\n"));
	const std::string messy = "class A {\n  function F(): number {\n        return g(1,\n  2) +\n3\n  }\n}\n";
	const std::string once = Formatted(messy);
	CHECK_EQ(Formatted(once), once);
	CHECK(!cluau::Format(once).Changed);
	cluau::FormatOptions spaces;
	spaces.Indent = "    ";
	CHECK_EQ(cluau::Format("if (a) {\nf()\n}\n", spaces).Text, std::string("if (a) {\n    f()\n}\n"));
}

TEST("format: a carriage return that does not end a line is space")
{
	const std::string once = Formatted("if (x) {\n\r \n\ty() \r\n\r\tz()\n}\n");
	CHECK_EQ(once, std::string("if (x) {\n\n\ty()\r\n\tz()\n}\n"));
	CHECK_EQ(Formatted(once), once);
}

TEST("format: a source that does not lex is left as it is")
{
	const cluau::FormatResult r = cluau::Format("let s = \"unterminated\n  let t = 1\n");
	CHECK(r.HasErrors());
	CHECK(!r.Changed);
	CHECK_EQ(r.Text, std::string("let s = \"unterminated\n  let t = 1\n"));
}

namespace {

	cluau::TranslateResult ScriptTranslation(const std::string& source)
	{
		cluau::TranslateOptions options;
		options.Script = true;
		options.Profile = &cluau::ScriptProfile();
		return cluau::Translate(source, options);
	}

} // namespace

TEST("language: throw raises a value as it is")
{
	// A script raises through the runtime (a catch's `error` may hide Luau's).
	CHECK_EQ(ScriptTranslation("throw \"bad\"\n").Luau, std::string("__cluau_throw( \"bad\")\n"));
	CHECK_EQ(ScriptTranslation("throw {\n\tcode = 1,\n}\n").Luau, std::string("__cluau_throw( {\n\tcode = 1,\n})\n"));
	// A host: Luau's error, with no position added, taken on the first line
	// before a local named `error` can hide it.
	CHECK_EQ(Luau("throw `x {y}`\n"), std::string("local __cluau_error = error; __cluau_error( `x {y}`, 0)\n"));
	const std::string shadowed = Luau("function F(error: string) {\n\ttry {\n\t\tthrow error\n\t} catch (error) {\n\t\tthrow error\n\t}\n}\n");
	CHECK_EQ(Lines(shadowed), 7);
	CHECK(shadowed.find("__cluau_error( error, 0)") != std::string::npos);
	CHECK_EQ(shadowed.find("= error"), (size_t)20); // `error` is read once, on the first line
	const cluau::TranslateResult mapped = cluau::Translate("x = 1\nthrow x\n");
	CHECK_EQ(mapped.Map.ToLuau({ 0, 0 }).Col, 29);
	CHECK_EQ(mapped.Map.FromLuau({ 0, 29 }).Col, 0);
	// `throw` as a name stays a name.
	CHECK_EQ(Luau("throw = 1\nthrow.x = 2\n"), std::string("throw = 1\nthrow.x = 2\n"));
	CHECK(HasError(cluau::Translate("throw\n"), "throw raises a value"));
}

TEST("language: every line stays on its line, with a string type continued on the next and after mistakes")
{
	// A field typed by a string that continues on the next line: the class
	// line's type says the line break as `\n`.
	const std::string continued = "class P {\n\tX: \"a\\\nb\" = \"a\\\nb\"\n}\nprint(P().X)\n";
	const cluau::TranslateResult r = ScriptTranslation(continued);
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), Lines(continued));
	CHECK(r.Luau.find("{ X: \"a\\nb\", }") != std::string::npos);
	// A long string a mistake drops keeps its lines.
	for (const std::string source : { std::string("class Square {\n[[x\ny\n"), std::string("class A {\n\tX: { string` = 1\n2`\n}\nlet b = 2\n") })
	{
		CHECK_EQ(Lines(cluau::Translate(source).Luau), Lines(source));
		CHECK_EQ(Lines(ScriptTranslation(source).Luau), Lines(source));
	}
}

TEST("language: try / catch / finally keep every line on its line")
{
	const std::string source = "function F(): number {\n\ttry {\n\t\treturn 1\n\t} catch (error) {\n\t\tprint(error)\n\t\treturn 2\n\t} finally {\n\t\tprint(3)\n\t}\n}\n";
	const cluau::TranslateResult r = ScriptTranslation(source);
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), Lines(source));
	CHECK(r.Luau.find("pcall(function(): ...any") != std::string::npos);
	CHECK(r.Luau.find("pcall(function (error: unknown): ...any") != std::string::npos);
	CHECK(r.Luau.find("return 1, table.pack( 1)") != std::string::npos);
	// The try ends its function: its return is unconditional.
	CHECK(r.Luau.find(" return table.unpack(__try1val or {}, 1,") != std::string::npos);
	// A catch's annotation is kept.
	CHECK(ScriptTranslation("try {\n} catch (error: string) {\n}\n").Luau.find("(error: string): ...any") != std::string::npos);
}

TEST("language: the lines holding only a try's own syntax are listed, for a debugger")
{
	const std::string source = "try {\n"                                // 0
							   "\ta()\n"                                // 1
							   "} catch (e) { // what went wrong\n"     // 2
							   "\tb()\n"                                // 3
							   "} finally {\n"                          // 4
							   "\tc()\n"                                // 5
							   "}\n"                                    // 6
							   "try { a() } catch (e: string) { b() }\n" // 7
							   "try {\n"                                // 8
							   "\ta()\n"                                // 9
							   "}\n"                                    // 10
							   "catch (e: { x: number,\n"               // 11
							   "\ty: number }) {\n"                     // 12
							   "}\n";                                   // 13
	const cluau::TranslateResult r = ScriptTranslation(source);
	CHECK(!r.HasErrors());
	CHECK(r.TryLines == std::vector<int>({ 2, 4, 6, 10, 11, 12, 13 }));
	CHECK(r.TryFunctionLines == std::vector<int>({ 0, 2, 7, 8, 11 }));
}

TEST("language: break and continue leave a try for the loop around it")
{
	const cluau::TranslateResult r = ScriptTranslation("for (i = 1, 3) {\n\ttry {\n\t\tbreak\n\t} catch (error) {\n\t\tcontinue\n\t}\n}\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("return 2") != std::string::npos);
	CHECK(r.Luau.find("return 3") != std::string::npos);
	CHECK(r.Luau.find("if __try1sig == 2 then break end") != std::string::npos);
	CHECK(r.Luau.find("if __try1sig == 3 then continue end") != std::string::npos);
	// A loop inside the try is the break's own.
	CHECK(ScriptTranslation("try {\n\tfor (i = 1, 2) {\n\t\tbreak\n\t}\n} finally {\n}\n").Luau.find("return 2") == std::string::npos);
}

TEST("language: try mistakes are reported")
{
	CHECK(HasError(ScriptTranslation("try {\n}\n"), "try needs a catch or a finally"));
	CHECK(HasError(ScriptTranslation("function F() {\n\ttry {\n\t} finally {\n\t\treturn\n\t}\n}\n"), "cannot return"));
	CHECK(HasError(ScriptTranslation("try {\n\tbreak\n} catch (error) {\n}\n"), "not inside a loop"));
	CHECK(HasError(ScriptTranslation("try {\n} catch {\n}\n"), "catch names the error"));
	// `try` as a name, and a call with a table, outside a statement start.
	CHECK(!cluau::Translate("try = 1\nlocal x = try\n").HasErrors());
}

TEST("language: enums are frozen tables with a type")
{
	const cluau::TranslateResult r = ScriptTranslation("enum Color { Red, Green = \"green\" }\nenum Code {\n\tOk = 200\n\tGone = -1\n}\n");
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), 5);
	CHECK(r.Luau.find("local Color = table.freeze( { Red = \"Red\" :: \"Red\", Green = \"green\" :: \"green\" }); export type Color = \"Red\" | \"green\"") !=
		std::string::npos);
	CHECK(r.Luau.find("Ok = 200,") != std::string::npos); // a comma where a line break separated members
	CHECK(r.Luau.find("export type Code = number") != std::string::npos);
	CHECK(HasError(ScriptTranslation("enum E { A, A }\n"), "has 'A' twice"));
	CHECK(HasError(ScriptTranslation("enum E { A = f() }\n"), "a string or a number"));
	CHECK(HasError(ScriptTranslation("enum E { }\n"), "has no members"));
	// Inside a block, the type is local to it.
	CHECK(ScriptTranslation("do {\n\tenum E { A }\n} while (false);\n").Luau.find("; type E =") != std::string::npos);
}

TEST("language: abstract classes and methods")
{
	const cluau::TranslateResult r = ScriptTranslation(
		"abstract class Shape {\n\tabstract function Area(): number\n\tabstract function Scale(by: number)\n}\n"
		"class Square extends Shape {\n\tfunction Area(): number { return 1 }\n\tfunction Scale(by: number) { }\n}\n");
	CHECK(!r.HasErrors());
	CHECK(r.Classes[0].Abstract);
	CHECK(r.Classes[0].Methods[0].Abstract);
	CHECK(r.Luau.find("__abstract = true, __abstracts = { \"Area\", \"Scale\" }") != std::string::npos);
	CHECK(r.Luau.find("function Shape.Area(self: Shape): number return __cluau_abstract(\"Shape\", \"Area\") end") != std::string::npos);
	CHECK(r.Luau.find("function Shape.Scale(self: Shape, by: number) __cluau_abstract(\"Shape\", \"Scale\") end") != std::string::npos);
	// An abstract class is not callable to the checker.
	CHECK(r.Luau.find("__cluau_meta :: { __iter: (unknown) -> ((unknown, string?) -> (string?, unknown), unknown) })") != std::string::npos);

	CHECK(HasError(ScriptTranslation("abstract class A {\n\tabstract function F()\n}\nclass B extends A {\n}\n"),
		"B does not define F, which A declares abstract"));
	CHECK(HasError(ScriptTranslation("class A {\n\tabstract function F()\n}\n"), "so A is too"));
	CHECK(HasError(ScriptTranslation("abstract class A {\n\tabstract function F() { }\n}\n"), "has no body"));
	CHECK(HasError(cluau::Translate("abstract class A {\n}\n"), "not abstract"));
}

TEST("statements: a line after x++, x--, `...` or a type starts a new statement")
{
	// The `if` is a statement, not an if-expression after an operator.
	CHECK(!cluau::Translate("let x = 1\nx++\nif (x > 0) { print(x) }\n").HasErrors());
	CHECK(!cluau::Translate("let n = 1\nn--\nif (n > 0) { print(n) }\n").HasErrors());
	CHECK(!cluau::Translate("let best: string?\nif (true) { print(best) }\n").HasErrors());
	CHECK(!cluau::Translate("type T = Array<number>\nif (true) { print(1) }\n").HasErrors());
	CHECK(!cluau::Translate("type M<K> = Map<K, Array<number>>\nif (true) { print(1) }\n").HasErrors());
	CHECK(!cluau::Translate("let m: Map<string, number>, k: number?\nif (true) { print(1) }\n").HasErrors());
	CHECK(!cluau::Translate("let v = {} :: Array<number>\nif (true) { print(v) }\n").HasErrors());
	CHECK(!cluau::Translate("function g(...: number) {\n\tlet a = ...\n\tif (a) { print(a) }\n}\n").HasErrors());
	CHECK_EQ(Luau("x++\nif (x) { }\n"), std::string("x += 1\nif (x) then end\n"));
	// A `{` starting the next line is a block, and try, throw and break are statements.
	CHECK_EQ(Luau("let n = 0\nn--\n{ print(n) }\n"), std::string("local n = 0\nn -= 1\ndo print(n) end\n"));
	CHECK(ScriptTranslation("let n = 0\nn++\ntry {\n\tprint(n)\n} catch (e) { }\n").Luau.find("pcall(") != std::string::npos);
	CHECK(ScriptTranslation("let s: string?\nthrow \"x\"\n").Luau.find("__cluau_throw( \"x\")") != std::string::npos);
	CHECK(!ScriptTranslation("while (true) {\n\tlet t: Array<number>\n\tbreak\n}\n").HasErrors());
	// A `>` that compares still waits for its right side.
	CHECK_EQ(Luau("let b = a >\n\tif (c) 1 else 2\n"), std::string("local b = a >\n\tif (c) then 1 else 2\n"));
	// Inside parentheses a `{` on the next line is a table argument.
	CHECK_EQ(Luau("print(fn\n{ a = 8 })\n"), std::string("print(fn\n{ a = 8 })\n"));
	CHECK_EQ(ScriptTranslation("let a = true\nthrow if (a)\n\t\"x\"\nelse\n\t\"y\"\n").Luau,
		std::string("local a = true\n__cluau_throw( if (a) then\n\t\"x\"\nelse\n\t\"y\")\n"));
}

TEST("classes: field defaults are translated in place, every line on its line")
{
	const std::string source =
		"class C {\n"
		"\tS: string = \"a    b\"\n"
		"\tL: string = [[x\n"
		"y]]\n"
		"\tT: {number} = {\n"
		"\t\t1, // one, and\n"
		"\t\t2,\n"
		"\t}\n"
		"\tCb: () -> number = function(): number { return 3 }\n"
		"\tY: number = 1 ~/ 2 // half\n"
		"\tfunction F(): number { return 1 }\n"
		"}\n";
	const cluau::TranslateResult r = cluau::Translate(source);
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), Lines(source));
	CHECK(r.Luau.find("\tC.Fields.S = \"a    b\"\n") != std::string::npos);
	CHECK(r.Luau.find("\tC.Fields.L = [[x\ny]]\n") != std::string::npos);
	CHECK(r.Luau.find("\tC.Fields.T = {\n\t\t1, -- one, and\n\t\t2,\n\t}\n") != std::string::npos);
	CHECK(r.Luau.find("\tC.Fields.Cb = function(): number  return 3 end\n") != std::string::npos);
	CHECK(r.Luau.find("\tC.Fields.Y = 1 // 2 -- half\n") != std::string::npos);
	// The method after them is on its own line (the 11th).
	CHECK_EQ(Lines(r.Luau.substr(0, r.Luau.find("\tfunction C.F(self: C): number "))), 10);
	// The profile gets the default translated.
	CHECK_EQ(r.Class->Fields[3].Default, std::string("function(): number  return 3 end"));
	CHECK_EQ(r.Class->Fields[0].Default, std::string("\"a    b\""));
	// The script profile evaluates each default afresh, around the same tokens.
	const cluau::TranslateResult s = ScriptTranslation("class D {\n\tQ: {number} = {\n\t\t1,\n\t}\n}\n");
	CHECK(!s.HasErrors());
	CHECK(s.Luau.find("\tD.Fields.Q = function() return {\n\t\t1,\n\t} end\n") != std::string::npos);
}

TEST("classes: a static's function value is a function, comments in a method's parameters")
{
	const cluau::TranslateResult r = cluau::Translate("class K {\n\tstatic Make = function(n: number): number { return n }\n}\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("K.Make = function(n: number): number  return n end") != std::string::npos);
	const cluau::TranslateResult m = cluau::Translate(
		"class P {\n\tfunction Init(\n\t\tx: number, // the x, really\n\t\ty: number /* the y */\n\t): number /* sum */ {\n\t\treturn x + y\n\t}\n}\n");
	CHECK(!m.HasErrors());
	CHECK_EQ(m.Class->Methods[0].Params, std::string("x: number, y: number"));
	CHECK_EQ(m.Class->Methods[0].ReturnType, std::string("number"));
	CHECK(m.Luau.find("Init: (self: P, x: number, y: number) -> number, }") != std::string::npos);
}

TEST("classes: generic methods keep their type parameters")
{
	const cluau::TranslateResult r = cluau::Translate("class B {\n\tfunction Id<U>(u: U): U {\n\t\treturn u\n\t}\n}\n");
	CHECK(r.Luau.find("Id: <U>(self: B, u: U) -> U, }") != std::string::npos);
	const cluau::TranslateResult s = ScriptTranslation("class B {\n\tfunction Init<T>(x: T) {\n\t}\n}\n");
	CHECK(s.Luau.find("__call: <T>(unknown, x: T) -> B") != std::string::npos);
}

TEST("language: a try's return and throw take the whole expression")
{
	const cluau::TranslateResult r = ScriptTranslation(
		"function F(): number {\n\ttry {\n\t\treturn 1\n\t\t\t+ 2\n\t} catch (e) { }\n\treturn 0\n}\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("return 1, table.pack( 1\n\t\t\t+ 2)") != std::string::npos);
	CHECK(ScriptTranslation("function G(): number {\n\ttry {\n\t\treturn 1\n\t\t\t- 2\n\t} catch (e) { }\n\treturn 0\n}\n")
			  .Luau.find("table.pack( 1\n\t\t\t- 2)") != std::string::npos);
	const cluau::TranslateResult i = ScriptTranslation(
		"function H(a: boolean): number {\n\ttry {\n\t\treturn if (a)\n\t\t\t1\n\t\telse if (!a)\n\t\t\t2\n\t\telse\n\t\t\t3\n\t} catch (e) { }\n\treturn 0\n}\n");
	CHECK(!i.HasErrors());
	CHECK(i.Luau.find("else\n\t\t\t3)\n") != std::string::npos);
	CHECK(ScriptTranslation("try {\n\tthrow \"a\"\n\t\t.. \"b\"\n} catch (e) { }\n").Luau.find("__cluau_throw( \"a\"\n\t\t.. \"b\")") !=
		std::string::npos);
	// A cast's type ending in `>` ends the expression with it.
	CHECK(ScriptTranslation("function J(): {number} {\n\ttry {\n\t\treturn {} :: Array<number>\n\t} catch (e) { }\n\tprint(1)\n\treturn {}\n}\n")
			  .Luau.find("table.pack( {} :: Array<number>)") != std::string::npos);
}

TEST("language: try and catch blocks see their function's varargs")
{
	const cluau::TranslateResult r = ScriptTranslation(
		"function F(...: number) {\n\ttry {\n\t\tprint(...)\n\t} catch (e) {\n\t\tprint(e, ...)\n\t}\n\ttry {\n\t\tlet f = function(...: number) { print(...) }\n\t} finally { }\n}\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("pcall(function(...: number): ...any \n\t\tprint(...)\n\tend, ...)") != std::string::npos);
	CHECK(r.Luau.find("pcall(function (e: unknown, ...: number): ...any \n\t\tprint(e, ...)\n\tend, __try1sig, ...) end") != std::string::npos);
	// Varargs only a nested function uses, or a type pack, are not the block's.
	CHECK(r.Luau.find("__try2val: any = pcall(function(): ...any") != std::string::npos);
	CHECK(ScriptTranslation("function G() {\n\ttry {\n\t\tlet t: (...number) -> () = print\n\t} finally { }\n}\n")
			  .Luau.find("pcall(function(): ...any") != std::string::npos);
}

TEST("language: a try inside a finally cannot leave the finally either")
{
	CHECK(HasError(ScriptTranslation("function F(): number {\n\ttry {\n\t} finally {\n\t\ttry { return 3 } catch (e) { }\n\t}\n\treturn 4\n}\n"),
		"cannot return"));
	CHECK(HasError(ScriptTranslation("while (true) {\n\ttry {\n\t} finally {\n\t\ttry { break } catch (e) { }\n\t}\n}\n"), "cannot break"));
	CHECK(HasError(ScriptTranslation("while (true) {\n\ttry {\n\t} finally {\n\t\ttry {\n\t\t\ttry { continue } finally { }\n\t\t} catch (e) { }\n\t}\n}\n"),
		"cannot continue"));
	// A loop inside the finally is the jump's own.
	CHECK(!ScriptTranslation("try {\n} finally {\n\tfor (i = 1, 2) {\n\t\ttry { break } catch (e) { }\n\t}\n}\n").HasErrors());
}

TEST("language: try's block may open on the next line, for takes local")
{
	const cluau::TranslateResult r = ScriptTranslation("try\n{\n\terror(\"x\")\n}\ncatch (e)\n{\n\tprint(e)\n}\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("pcall(") != std::string::npos);
	CHECK_EQ(Luau("for (local i = 1, 2) { f(i) }"), std::string("for   i = 1, 2  do f(i) end"));
	// A statement after the condition is not the block (a later `{` is not taken for it).
	const cluau::TranslateResult b = cluau::Translate("if (x) print(\"a\")\nlet t = {}\n");
	CHECK(HasError(b, "if needs a { } block"));
	CHECK(!HasError(b, "Expected"));
	CHECK(!cluau::Translate("if (a) && (b) { }\n").HasErrors());
}

TEST("comments: line comments that look like long brackets, block comments ending in ]")
{
	CHECK_EQ(Luau("//[[ not long\nprint(1)\n//[==[ nor this\n"), std::string("-- [[ not long\nprint(1)\n-- [==[ nor this\n"));
	CHECK_EQ(Luau("/* see t[1]*/ x()"), std::string("--[=[ see t[1]]=] x()"));
	CHECK_EQ(Luau("/* x]=*/ x()"), std::string("--[[ x]=]] x()"));
	// A `//!` after the code starts is an ordinary comment; before it, a mode comment.
	CHECK(!cluau::Translate("print(1) //! note\n//!! banner\n").HasErrors());
	CHECK(HasError(cluau::Translate("//!nonstrict\nprint(1)\n"), "no mode comments"));
}

TEST("strings: nested interpolations, CRLF after a backslash")
{
	CHECK_EQ(Luau("print(`x {`y {a != 1 && !b}`} {`}`}`)\n"), std::string("print(`x {`y {a ~= 1  and  not b}`} {`}`}`)\n"));
	const cluau::TranslateResult r = cluau::Translate("let s = \"a\\\r\nb\"\r\nprint(s)\r\n");
	CHECK(!r.HasErrors());
	CHECK_EQ(Lines(r.Luau), 3);
	// Nesting deeper than any program needs is reported, not a crash.
	std::string deep = "print(";
	for (int i = 0; i < 100000; i++)
		deep += "`{";
	deep += "1";
	for (int i = 0; i < 100000; i++)
		deep += "}`";
	deep += ")\n";
	CHECK(HasError(cluau::Translate(deep), "nest too deeply"));
	CHECK(cluau::Format(deep).HasErrors());
}

TEST("enums: a mixed enum's type is its strings and number")
{
	const cluau::TranslateResult r = ScriptTranslation("enum M { A = \"x\", B, C = -1 }\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("type M = \"x\" | \"B\" | number") != std::string::npos);
}

TEST("types: a long chain of function types is read without recursion")
{
	std::string chain = "let f: ";
	for (int i = 0; i < 2000000; i++)
		chain += "() -> ";
	chain += "number\nif (true) { print(1) }\n";
	const cluau::TranslateResult r = cluau::Translate(chain);
	CHECK(!r.HasErrors());
}

TEST("format: a generic's closing > ends a line, every line keeps its own line break")
{
	CHECK_EQ(Formatted("type T = Array<number>\nlet x = 1\n"), std::string("type T = Array<number>\nlet x = 1\n"));
	CHECK_EQ(Formatted("let m: Map<string, Array<number>>\nlet x = 1\n"), std::string("let m: Map<string, Array<number>>\nlet x = 1\n"));
	CHECK_EQ(Formatted("let c = a >\nb\n"), std::string("let c = a >\n\tb\n"));
	// A CRLF line, and a long string with both kinds inside: each as it was.
	CHECK_EQ(Formatted("let a = 1\r\nlet s = [[x\ny\r\nz]]\nlet b = 2\n"), std::string("let a = 1\r\nlet s = [[x\ny\r\nz]]\nlet b = 2\n"));
	CHECK_EQ(Formatted("let a = 1\r\nlet b = 2"), std::string("let a = 1\r\nlet b = 2\r\n"));
}

TEST("classes: a field's default is placed by the lowering's shape, not found in its text")
{
	// The default's text occurs earlier in the lowered line (`3` in Vector3, `d` in Fields).
	const std::string vec = ScriptTranslation("class Vector3 {\n\tZ: number = 3\n}\n").Luau;
	CHECK(vec.find("\tVector3.Fields.Z = function() return 3 end\n") != std::string::npos);
	const std::string box = ScriptTranslation("const d = 5\nclass Box {\n\tWidth: number = d\n}\n").Luau;
	CHECK(box.find("\tBox.Fields.Width = function() return d end\n") != std::string::npos);
	const std::string host = Luau("class Vec2 {\n\tY: number = 2\n}\n");
	CHECK(host.find("\tVec2.Fields.Y = 2\n") != std::string::npos);
	// A profile whose lowering does not hold the default as it is: one line.
	cluau::ClassProfile upper;
	upper.LowerField = [](const cluau::ClassField& field, const std::string& className, std::string&) {
		return className + ".Fields." + field.Name + " = " + std::to_string(field.Default.size());
	};
	cluau::TranslateOptions options;
	options.Profile = &upper;
	CHECK(Luau("class P {\n\tS: string = \"abc\"\n}\n", options).find("\tP.Fields.S = 5 ") != std::string::npos);
}

TEST("classes: positions in a field's default map to the source")
{
	const cluau::TranslateResult r = ScriptTranslation("class K {\n\tF: number = ( nope )\n\tH: number = nope3 + 1\n}\n");
	CHECK(!r.HasErrors());
	auto luauColumn = [&](int line, const std::string& word) {
		size_t start = 0;
		for (int l = 0; l < line; l++)
			start = r.Luau.find('\n', start) + 1;
		return (int)(r.Luau.find(word, start) - start);
	};
	CHECK_EQ(r.Map.FromLuau({ 1, luauColumn(1, "nope") }).Col, 15);
	CHECK_EQ(r.Map.FromLuau({ 2, luauColumn(2, "nope3") }).Col, 13);
}

TEST("classes: a loop over a script's class names its members' types")
{
	const std::string loop = ScriptTranslation("class C {\n\tstatic N = 1\n}\nfor (k, v in C) {\n}\nfor (k in C) {\n}\n").Luau;
	CHECK(loop.find("for  k: string, v: unknown in C") != std::string::npos);
	CHECK(loop.find("for  k: string in C") != std::string::npos);
	CHECK(loop.find("__iter: (unknown) -> ((unknown, string?) -> (string?, unknown), unknown)") != std::string::npos);
	// The name may be another value's where the loop is: no annotations.
	CHECK(ScriptTranslation("class C {\n}\nfunction F(C: { number }) {\n\tfor (i, v in C) {\n\t}\n}\n").Luau.find(": string") == std::string::npos);
	CHECK(ScriptTranslation("class C {\n}\ndo {\n\tlet C = { 1 }\n\tfor (i, v in C) {\n\t}\n}\n").Luau.find(": string") == std::string::npos);
	// Annotated variables, a call, a class declared later: left alone.
	CHECK(ScriptTranslation("class C {\n}\nfor (k: string in C) {\n}\n").Luau.find("k: string: string") == std::string::npos);
	CHECK(ScriptTranslation("class C {\n}\nfor (k in pairs(C)) {\n}\n").Luau.find("k: string") == std::string::npos);
	CHECK(ScriptTranslation("for (k in C) {\n}\nclass C {\n}\n").Luau.find("k: string") == std::string::npos);
}

TEST("classes: a script class's `};` after a member's own `;`")
{
	const cluau::TranslateResult r = ScriptTranslation("class Point {\n\tX: number = 1;\n};\nlet p = Point()\n");
	CHECK(!r.HasErrors());
	CHECK(r.Luau.find("end;\n\n") != std::string::npos);
	CHECK(r.Luau.find("\n;") == std::string::npos);
	// Without one of its own, the class's `;` stays (it ends the statement before a `(` line).
	CHECK(ScriptTranslation("class Q {\n\tstatic F = f\n};\n(g)()\n").Luau.find("Q.F = f\n;\n") != std::string::npos);
}

TEST("comments: a comment right after a minus stays a comment of its own")
{
	CHECK_EQ(Luau("x = x -/* one */1"), std::string("x = x - --[[ one ]]1"));
	CHECK_EQ(Luau("y = 10 -// more\n3"), std::string("y = 10 - -- more\n3"));
	CHECK_EQ(Luau("z = 1 - /* c */ 2"), std::string("z = 1 - --[[ c ]] 2"));
}

TEST("interpolation: a segment is translated like any other expression")
{
	CHECK_EQ(Luau("s = `{(function() { return 1 })()}`"), std::string("s = `{(function()  return 1 end)()}`"));
	CHECK_EQ(Luau("s = `{a /* n */ + b}`"), std::string("s = `{a --[[ n ]] + b}`"));
	CHECK_EQ(Luau("s = `{a and!b}`"), std::string("s = `{a and not b}`"));
	CHECK_EQ(Luau("s = `{string.len([==[a`b]==])}`"), std::string("s = `{string.len([==[a`b]==])}`"));
	CHECK_EQ(Luau("s = `{if (a) 1 else 2} and {x ~/ 2}`"), std::string("s = `{if (a) then 1 else 2} and {x // 2}`"));
	CHECK_EQ(Luau("s = `a{`b{c != d}`}e{f}g`"), std::string("s = `a{`b{c ~= d}`}e{f}g`"));
	CHECK_EQ(Luau("s = `{t[\"}\"]} \\{x}`"), std::string("s = `{t[\"}\"]} \\{x}`"));
	// `//` starts a comment there too (floor division is `~/`), which here runs past the string's end.
	CHECK(HasError(cluau::Translate("s = `{a // b}`\n"), "unterminated interpolated string"));
	// An if-expression in a segment needs its else before the segment ends.
	CHECK(HasError(cluau::Translate("s = `{if (a) 1}`\n"), "an if-expression needs an else"));
	CHECK(HasError(cluau::Translate("s = `{f(}`\n"), "unclosed '('"));
	// Its lines and columns are the source's.
	const std::string multi = "s = `{a +\n\tb}` .. `{c\n}`\nprint(s)\n";
	CHECK_EQ(Lines(Luau(multi)), Lines(multi));
	const cluau::TranslateResult r = cluau::Translate("s = `x{a != b}y` .. `{!c}`");
	CHECK_EQ(r.Luau, std::string("s = `x{a ~= b}y` .. `{not c}`"));
	CHECK_EQ(r.Map.ToLuau({ 0, 11 }).Col, 11);
	CHECK_EQ(r.Map.ToLuau({ 0, 23 }).Col, 26);
}

TEST("interpolation: a try passes on the `...` a segment uses")
{
	const std::string luau = Luau("function f(...: number) {\n\ttry {\n\t\tprint(`{select(\"#\", ...)}`)\n\t} catch (e) {\n\t}\n}\n");
	CHECK(luau.find("pcall(function(...: number): ...any") != std::string::npos);
	CHECK(luau.find("end, ...)") != std::string::npos);
}

TEST("try: a return's value on the next line is the return's, as Luau reads it")
{
	const std::string luau = Luau("function f(): number {\n\ttry {\n\t\treturn\n\t\t\t42\n\t} catch (e) {\n\t\treturn 0\n\t}\n}\n");
	CHECK(luau.find("return 1, table.pack(\n\t\t\t42)") != std::string::npos);
	CHECK(Luau("function g() {\n\ttry {\n\t\treturn\n\t} catch (e) {\n\t}\n}\n").find("return 1, table.pack()\n") != std::string::npos);
}

TEST("scripts: REPL declarations whose type does not end are left for Luau")
{
	cluau::TranslateOptions options;
	options.Repl = true;
	CHECK_EQ(Luau("let x: Array<number = {1, 2}; print(\"after\")", options), std::string("local x: Array<number = {1, 2}; print(\"after\")"));
	CHECK_EQ(Luau("let y: {number = 1", options), std::string("local y: {number = 1}"));
	// A blanked type keeps the lines of a token that spans them.
	const std::string typed = "let s: \"a\\\nb\" = \"a\\\nb\"\nprint(s)\n";
	CHECK_EQ(Lines(Luau(typed, options)), Lines(typed));
}

TEST("scripts: a pending if-expression or a continued string is unfinished")
{
	CHECK(cluau::Translate("let y = if (true)\n").Unfinished);
	CHECK(cluau::Translate("let y = if (a) 1 else if (b)\n").Unfinished);
	CHECK(cluau::Translate("let s = \"a\\\n").Unfinished);
	CHECK(cluau::Translate("let s = \"a\\").Unfinished);
	CHECK(cluau::Translate("let s = \"a\\z  \n").Unfinished);
	CHECK(cluau::Translate("let s = `a{`b{x").Unfinished);
	CHECK(!cluau::Translate("let y = if (a) 1 else 2\n").Unfinished);
	CHECK(!cluau::Translate("let s = \"a\\n\n").Unfinished);
}

TEST("sources: a UTF-8 byte order mark is not code")
{
	const cluau::TranslateResult r = cluau::Translate("\xEF\xBB\xBFlet x = 1\nprint(x)\n");
	CHECK(!r.HasErrors());
	CHECK_EQ(r.Luau, std::string("local x = 1\nprint(x)\n"));
	CHECK_EQ(r.Map.ToLuau({ 0, 11 }).Col, 10);
	CHECK_EQ(Formatted("\xEF\xBB\xBF  let x = 1\n"), std::string("\xEF\xBB\xBFlet x = 1\n"));
	CHECK_EQ(Formatted("\xEF\xBB\xBFif (a) {\nf()\n}\n"), std::string("\xEF\xBB\xBFif (a) {\n\tf()\n}\n"));
}

TEST("diagnostics: the first fifty, and how many more")
{
	const cluau::TranslateResult r = cluau::Translate(std::string(200, '}'));
	CHECK_EQ(r.Diagnostics.size(), (size_t)51);
	CHECK(r.Diagnostics.back().Message.find("150 more errors") == 0);
	CHECK_EQ(r.Diagnostics.back().At.Begin.Col, 50);
}

TEST("statements: a brace-less if does not take a later line's table for its block")
{
	CHECK(HasError(cluau::Translate("if (x) (f)()\nlet t = { 1 }\n"), "if needs a { } block"));
	CHECK(HasError(cluau::Translate("while (x) (f)()\nlet t = { 1 }\n"), "while needs a { } block"));
	CHECK(!cluau::Translate("if (a &&\n\tb) {\n}\nif (a)\n{\n}\nwhile (a) // why\n{\n}\nif (a)\n\t&& (b) {\n}\n").HasErrors());
}

TEST("tables: an if-expression after a `;` separator; form feeds are whitespace")
{
	CHECK_EQ(Luau("let t = { 1; if (a) 2 else 3 }"), std::string("local t = { 1; if (a) then 2 else 3 }"));
	CHECK(!cluau::Translate("let t = { 1; if (a) 2 else 3 }\n").HasErrors());
	CHECK_EQ(Luau("let x =\f1\v+ 2"), std::string("local x =\f1\v+ 2"));
	CHECK(!cluau::Translate("let x =\f1\v+ 2\n").HasErrors());
}

TEST("nil operators: ?? and ?. test a name where it is, and call a helper for anything else")
{
	CHECK_EQ(Luau("let x = a ?? b"), std::string("local x = (if a  ~= nil then a else  b)"));
	CHECK_EQ(Luau("let x = a?.b.c"), std::string("local x = (if a == nil then nil else a.b.c)"));
	CHECK_EQ(Luau("a?:close()"), std::string("if a ~= nil then a:close() end"));
	CHECK_EQ(Luau("x ??= 1"), std::string("if x  == nil then x = 1 end"));
	const std::string called = Luau("let x = f() ?? 0\nlet y = g()?.z\n");
	CHECK(called.find("(__cluau_or(f() ,  0) :: typeof(assert(f())) | typeof(0))") != std::string::npos);
	CHECK(called.find("__cluau_chain(g(), function(__cluau_v: typeof(assert(g()))) return __cluau_v.z end)") != std::string::npos);
	// The helpers are defined on the first line; every line stays on its line.
	CHECK_EQ(Lines(called), 2);
	CHECK(called.rfind("local function __cluau_chain", 0) == 0 || called.rfind("local function __cluau_or", 0) == 0);
	CHECK_EQ(Lines(Luau("let x = f()\n\t?? g()\n\t?? 3\n")), 3);
	CHECK(HasError(cluau::Translate("a?.b = 1"), "cannot be assigned to"));
	CHECK(HasError(cluau::Translate("let x = ?? 1"), "?? goes between two values"));
	CHECK(HasError(cluau::Translate("f(x) ??= 1"), "??= is a statement"));
	// A type's `?` is not a chain.
	CHECK(!cluau::Translate("let x: string? = nil\nfunction f(a: number?): {string}? { return nil }\n").HasErrors());
}

TEST("parameters: a default makes the parameter optional and is given at the body's start")
{
	CHECK_EQ(Luau("function f(a: number, b: number = 2) { return a + b }"),
		std::string("function f(a: number, b: number?  )  local b: number = if b == nil then 2 else b; return a + b end"));
	CHECK_EQ(Luau("function f(n = 1, s = \"x\", u: A | B = g()) { }"),
		std::string("function f(n: number?  , s: string?  , u: (A | B)?  )  local n: number = if n == nil then 1 else n; local s: string = if "
					"s == nil then \"x\" else s; local u: A | B = if u == nil then g() else u; end"));
	CHECK(HasError(cluau::Translate("function f(a = 1,\n\tb = (\n2)) { }"), "written on one line"));
	CHECK(HasError(cluau::Translate("function f(... = 1) { }"), "... has no default"));
}

TEST("generators: function* returns an iterator, and yield hands it a value")
{
	const std::string luau = Luau("function* f(n: number): number {\n\tyield n\n}\n");
	CHECK(luau.find("function f(n: number): () -> number?  return __cluau_generator(function(__cluau_yield: (number) -> ())\n\t__cluau_yield( n)\nend) end") !=
		std::string::npos);
	CHECK(Luau("function* f(): (string, number) { }").find("(): () -> (string?, number)   return __cluau_generator(function(__cluau_yield: (string, number) -> ()) end) end") != std::string::npos);
	CHECK(HasError(cluau::Translate("function* f() { return 1 }"), "a generator returns no value"));
	CHECK(HasError(cluau::Translate("function* f() {\n\tyield\n}"), "yield gives a value"));
	// Outside a generator, yield is a name.
	CHECK_EQ(Luau("yield(1)"), std::string("yield(1)"));
	CHECK_EQ(Luau("function f() { yield(1) }"), std::string("function f()  yield(1) end"));
}

TEST("destructuring: fields by name and items in order, a value read once")
{
	CHECK_EQ(Luau("const { a, b } = t"), std::string("local  a, b  = t.a, t.b"));
	CHECK_EQ(Luau("let [x, y] = p.list"), std::string("local x, y = p.list[1], p.list[2]"));
	CHECK_EQ(Luau("const { a } = f()"), std::string("local  a  = (function(__cluau_t: typeof(f())) return __cluau_t.a end)( f())"));
	const cluau::TranslateResult r = cluau::Translate("const { a, b } = t\n");
	CHECK_EQ(r.ConstNames.size(), (size_t)2);
	CHECK(HasError(cluau::Translate("const { 1 } = t"), "a { } pattern takes fields"));
	CHECK(HasError(cluau::Translate("const [a] = x, y"), "one value"));
	CHECK(HasError(cluau::Translate("const { a }"), "needs its value after ="));
}

TEST("destructuring patterns: renames, nesting, defaults and a list's rest")
{
	CHECK_EQ(Luau("const { name: n, pos: { x, y } } = t"), std::string("local   n,   x, y   = t.name, t.pos.x, t.pos.y"));
	CHECK_EQ(Luau("const [a, [b, c]] = t"), std::string("local a, b, c = t[1], t[2][1], t[2][2]"));
	CHECK_EQ(Luau("const { a, b = 2 } = t"),
		std::string("local  a, b    = (function(__cluau_t: typeof(t)) local a = __cluau_t.a; local __cluau_d2 = __cluau_t.b; local b = if "
					"__cluau_d2 ~= nil then __cluau_d2 else 2; return a, b end)( t)"));
	CHECK_EQ(Luau("const [first, ...rest] = list"),
		std::string("local first, rest = (function(__cluau_t: typeof(list)) local first = __cluau_t[1]; local rest = table.move(__cluau_t, 2, "
					"#__cluau_t, 1, {}); return first, rest end)( list)"));
	// A default may use the names before it, and a trailing comma goes.
	CHECK(Luau("let [a, b = a,] = t").find("local b = if __cluau_d2 ~= nil then __cluau_d2 else a;") != std::string::npos);
	CHECK(!cluau::Translate("let [a, b = a,] = t").HasErrors());
	// Where a pattern is, `[[` is two brackets; a long string elsewhere.
	CHECK_EQ(Luau("const [[a, b], c] = t"), std::string("local a, b, c = t[1][1], t[1][2], t[2]"));
	CHECK_EQ(Luau("let { p: [[x]] } = t"), std::string("local   x  = t.p[1][1]"));
	CHECK(Luau("for (_, [[a, b]] in t) { }").find("local a = __cluau_v2[1][1]; local b = __cluau_v2[1][2];") != std::string::npos);
	CHECK_EQ(Luau("local s = [[text]]\nf(a, [[x]])\nfor (_, v in f([[y]])) { }"), std::string("local s = [[text]]\nf(a, [[x]])\nfor  _, v in f([[y]])  do end"));
	CHECK(Luau("const [a = [[s]]] = t").find("else [[s]]; return a end") != std::string::npos);
	CHECK(HasError(cluau::Translate("const { a, ...rest } = t"), "the rest of a list is taken in a [ ] pattern"));
	CHECK(HasError(cluau::Translate("const [...rest, a] = t"), "a rest element is the pattern's last"));
	CHECK(HasError(cluau::Translate("const { pos: { x } = p } = t"), "a nested pattern has no default"));
	CHECK(HasError(cluau::Translate("const { a = } = t"), "a default needs a value"));
	CHECK(HasError(cluau::Translate("const { pos: {} } = t"), "a { } pattern takes fields"));
	const cluau::TranslateResult r = cluau::Translate("const { a: b, c: [d] } = t\n");
	CHECK_EQ(r.ConstNames.size(), (size_t)2);
}

TEST("loop patterns: a loop variable's fields or items are locals of the body")
{
	CHECK_EQ(Luau("for (_, { name, size = 1 } in items) {\n}"),
		std::string("for  _, __cluau_v2      in items  do local name = __cluau_v2.name; local __cluau_d2 = __cluau_v2.size; local size = if "
					"__cluau_d2 ~= nil then __cluau_d2 else 1;\nend"));
	CHECK_EQ(Luau("for ([k, v] in pairs) { }"), std::string("for  __cluau_v1   in pairs  do local k = __cluau_v1[1]; local v = __cluau_v1[2]; end"));
	CHECK(HasError(cluau::Translate("for ({ a } = 1, 2) { }"), "a numeric loop counts with a name"));
}

TEST("rest parameters and spreads: ...items in a signature, a call and a table")
{
	CHECK_EQ(Luau("function f(a, ...rest: number) { }"), std::string("function f(a, ...: number)  local rest: { number } = { ... }; end"));
	CHECK_EQ(Luau("function f(...rest) { }"), std::string("function f(...)  local rest = { ... }; end"));
	CHECK(HasError(cluau::Translate("function f(...rest, a) { }"), "a rest parameter is the last"));
	CHECK_EQ(Luau("f(a, ...items)"), std::string("f(a, table.unpack(items))"));
	CHECK_EQ(Luau("o:m(...items.list)"), std::string("o:m(table.unpack(items.list))"));
	CHECK(HasError(cluau::Translate("f(...items, a)"), "a spread argument goes last"));
	CHECK(Luau("local t = { 1, ...a, x = 2 }").find("(__cluau_spread({ 1, }, a, { x = 2 }) :: typeof(a) & typeof({ 1, x = 2 }))") != std::string::npos);
	CHECK(Luau("local t = { ...a }").find("(__cluau_spread({ }, a ) :: typeof(a))") != std::string::npos);
	CHECK(Luau("local t = { ...a, ...b }").find("(__cluau_spread({ }, a, { }, b ) :: typeof(a) & typeof(b))") != std::string::npos);
	// `...` as a value, and a type's `...T`, are as they were.
	CHECK_EQ(Luau("function f(...) { return { ... }, g(...) }"), std::string("function f(...)  return { ... }, g(...) end"));
	CHECK_EQ(Luau("local f: (...number) -> () = g"), std::string("local f: (...number) -> () = g"));
	CHECK_EQ(Luau("type F = (string, ...any) -> ...number"), std::string("type F = (string, ...any) -> ...number"));
}

TEST("async and await: a task per call, joined by await (scripts)")
{
	cluau::TranslateOptions script;
	script.Script = true;
	const std::string luau = cluau::Translate("async function f(n: number): string {\n\treturn await g(n)\n}", script).Luau;
	CHECK(luau.find("local __cluau_task = require(\"@std/task\"); ") == 0);
	CHECK(luau.find("function f(n: number): __cluau_task.Task<string>  return __cluau_task.async(function(): string\n\treturn "
					"__cluau_task.join( g(n))\nend) end") != std::string::npos);
	CHECK(cluau::Translate("async function f() { }", script).Luau.find("f(): __cluau_task.Task<()>  return __cluau_task.async(function() end) end") !=
		std::string::npos);
	CHECK(cluau::Translate("async function f(...: number) { return select(\"#\", ...) }", script)
			  .Luau.find("async(function(...: number) return select(\"#\", ...) end, ...) end") != std::string::npos);
	CHECK(cluau::Translate("local v = await a.b[1]:c() + 1", script).Luau.find("__cluau_task.join( a.b[1]:c()) + 1") != std::string::npos);
	CHECK(cluau::Translate("local v = await a?.b", script).Luau.find("__cluau_task.join( (if a == nil then nil else a.b))") != std::string::npos);
	// A name that is not a keyword's use stays a name, and hosts have neither.
	CHECK_EQ(cluau::Translate("local await = 1\nprint(await)", script).Luau, std::string("local await = 1\nprint(await)"));
	CHECK_EQ(Luau("local v = await(t)"), std::string("local v = await(t)"));
	CHECK(HasError(cluau::Translate("async function f() { }"), "only scripts have"));
	CHECK(HasError(cluau::Translate("async function* f() { }", script), "a generator is not async"));
}

TEST("nested ??, ?. and spreads: each level adds to the translation, not doubles it")
{
	cluau::TranslateOptions script;
	script.Script = true;
	std::string coalesce = "local x = f()", chain = "local x = f()", spread = "local x = ", close;
	for (int level = 0; level < 40; level++)
	{
		coalesce += " ?? (g()";
		close += ")";
		chain += "?.a";
		spread += "{ ...";
	}
	spread += "a";
	for (int level = 0; level < 40; level++)
		spread += " }";
	for (const std::string& source : { coalesce + close, chain, spread })
	{
		const cluau::TranslateResult r = cluau::Translate(source, script);
		CHECK(!r.HasErrors());
		CHECK(r.Luau.size() < 100000); // doubling would be terabytes
	}
}

TEST("?? and ?. in a parameter's default")
{
	CHECK_EQ(Luau("function f(a = b ?? 1) { }"), std::string("function f(a    )  local a = if a == nil then (if b  ~= nil then b else  1) else a; end"));
	CHECK(!cluau::Translate("function f(a = m?.n) { }").HasErrors());
}

TEST("switch: an if chain over the value, which a name is compared as")
{
	CHECK_EQ(Luau("switch (x) {\ncase 1, 2: f()\ncase 3:\ng()\ndefault: h()\n}"),
		std::string("do local __cluau_switch0 = (x) \nif x == ( 1) or x == ( 2) then f()\nelseif x == ( 3) then\ng()\nelse h()\nend end"));
	CHECK(Luau("switch (f()) { case 1: g() }").find("if __cluau_switch0 == ( 1) then g() end end") != std::string::npos);
	CHECK(HasError(cluau::Translate("for (i = 1, 2) { switch (i) { case 1: break } }"), "a switch needs no break"));
	CHECK(!cluau::Translate("for (i = 1, 2) { switch (i) { case 1: for (j = 1, 2) { break } } }").HasErrors());
	CHECK(HasError(cluau::Translate("switch (x) { f() case 1: }"), "a switch holds cases"));
	CHECK(HasError(cluau::Translate("switch (x) { default: case 1: }"), "default is a switch's last clause"));
	CHECK(HasError(cluau::Translate("switch (x) { case 1 }"), "a case is values and a colon"));
	// Elsewhere, switch, case and default are names.
	CHECK_EQ(Luau("local case, default = 1, 2"), std::string("local case, default = 1, 2"));
	CHECK_EQ(Formatted("switch (x) {\ncase 1:\nf()\ndefault:\ng()\n}\n"), std::string("switch (x) {\n\tcase 1:\n\t\tf()\n\tdefault:\n\t\tg()\n}\n"));
}
