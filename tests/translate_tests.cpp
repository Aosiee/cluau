#include "test.h"

#include "cluau/cluau.h"

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
