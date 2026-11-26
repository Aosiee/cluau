#include "test.h"

#include "cluau/convert.h"

#include <algorithm>

namespace {

	std::string Cluau(const std::string& luau)
	{
		return cluau::Convert(luau).Cluau;
	}

	// Converted, translated back, and token-identical to the input.
	bool RoundTrips(const std::string& luau)
	{
		const cluau::ConvertResult r = cluau::Convert(luau);
		const std::string problem = cluau::VerifyConversion(luau, r.Cluau);
		if (!problem.empty())
			std::fprintf(stderr, "    round trip: %s\n    cluau:\n%s\n", problem.c_str(), r.Cluau.c_str());
		return problem.empty() && !r.HasErrors();
	}

	bool HasError(const cluau::ConvertResult& r, const std::string& needle)
	{
		return std::any_of(r.Diagnostics.begin(), r.Diagnostics.end(),
			[&](const cluau::Diagnostic& d) { return d.Message.find(needle) != std::string::npos; });
	}

} // namespace

TEST("convert: blocks become braces, conditions get parentheses")
{
	CHECK_EQ(Cluau("if a then x() elseif b then y() else z() end"),
		std::string("if (a) { x() } else if (b) { y() } else { z() }"));
	CHECK_EQ(Cluau("if (a) then x() end"), std::string("if (a) { x() }"));
	CHECK_EQ(Cluau("while n > 0 do n -= 1 end"), std::string("while (n > 0) { n -= 1 }"));
	CHECK_EQ(Cluau("for i = 1, 10 do f(i) end"), std::string("for (i = 1, 10) { f(i) }"));
	CHECK_EQ(Cluau("for k, v in pairs(t) do f(k) end"), std::string("for (k, v in pairs(t)) { f(k) }"));
	CHECK_EQ(Cluau("do local y = 2 end"), std::string("{ let y = 2 }"));
	CHECK_EQ(Cluau("repeat n += 1 until n >= 3"), std::string("do { n += 1 } while (!(n >= 3));"));
	CHECK_EQ(Cluau("repeat n += 1 until not (n < 3)"), std::string("do { n += 1 } while (n < 3);"));
	CHECK_EQ(Cluau("repeat until x; f()"), std::string("do { } while (!(x)); f()"));
}

TEST("convert: functions, signatures and return types")
{
	CHECK_EQ(Cluau("function f(a: number): number return a end"), std::string("function f(a: number): number { return a }"));
	CHECK_EQ(Cluau("local function g() end"), std::string("local function g() { }"));
	CHECK_EQ(Cluau("local h = function(x) return x end"), std::string("let h = function(x) { return x }"));
	CHECK_EQ(Cluau("function M.Go(self: M, ...: number): (number, string) end"),
		std::string("function M.Go(self: M, ...: number): (number, string) { }"));
	CHECK_EQ(Cluau("function M:On(fn: (number) -> boolean): {number}? end"),
		std::string("function M:On(fn: (number) -> boolean): {number}? { }"));
	CHECK_EQ(Cluau("function id<T>(x: T): T return x end"), std::string("function id<T>(x: T): T { return x }"));
	CHECK_EQ(Cluau("function u(): Foo.Bar<number> | nil return nil end"),
		std::string("function u(): Foo.Bar<number> | nil { return nil }"));
}

TEST("convert: operators and comments")
{
	CHECK_EQ(Cluau("x = a ~= b and not c or d // 2"), std::string("x = a != b && !c || d ~/ 2"));
	CHECK_EQ(Cluau("x //= 2"), std::string("x ~/= 2"));
	CHECK_EQ(Cluau("-- note\n--- doc\nx = 1 -- tail"), std::string("// note\n/// doc\nx = 1 // tail"));
	CHECK_EQ(Cluau("--[[ a\nb ]]\nx = 1"), std::string("/* a\nb */\nx = 1"));
	CHECK_EQ(Cluau("--!strict\nx = \"-- not a comment\""), std::string("\nx = \"-- not a comment\""));
	CHECK_EQ(Cluau("s = [[if then end]] .. 'and'"), std::string("s = [[if then end]] .. 'and'"));
	CHECK_EQ(Cluau("t.end_ = obj:Do(1, 2)"), std::string("t.end_ = obj:Do(1, 2)"));
}

TEST("convert: what has no cluau form is reported")
{
	CHECK(HasError(cluau::Convert("local let = 1"), "cluau keyword"));
	CHECK(HasError(cluau::Convert("if a then"), "not closed"));
	CHECK(HasError(cluau::Convert("end"), "does not close"));
}

TEST("convert: the output keeps every line and round-trips")
{
	const std::string module =
		"--!strict\n"
		"-- A spinner.\n"
		"local Spinner = {}\n"
		"Spinner.Properties = { Speed = { type = \"number\", default = 90 } }\n"
		"\n"
		"function Spinner.OnFixedUpdate(self, dt: number)\n"
		"\tlocal t = self.Entity.Transform\n"
		"\tif t == nil or\n"
		"\t\tnot self.Enabled then\n"
		"\t\treturn\n"
		"\telseif self.Speed ~= 0 then\n"
		"\t\tt.Rotation = t.Rotation * 2\n"
		"\tend\n"
		"\tfor i, v in ipairs({ 1, 2 }) do\n"
		"\t\twhile v > 0 do v -= 1 end\n"
		"\tend\n"
		"\trepeat\n"
		"\t\tdt -= 1\n"
		"\tuntil dt <= 0\n"
		"\tWorld.After(1, function()\n"
		"\t\tprint(`done {dt}`)\n"
		"\tend)\n"
		"end\n"
		"\n"
		"return Spinner\n";
	const std::string out = Cluau(module);
	CHECK_EQ(std::count(out.begin(), out.end(), '\n'), std::count(module.begin(), module.end(), '\n'));
	CHECK(RoundTrips(module));
	CHECK(RoundTrips("local a = { b = function(x: number): number return x // 2 end }\nreturn a\n"));
}

TEST("convert: the corners Luau's conformance suite found")
{
	// A do-block right after an expression is not call sugar.
	CHECK_EQ(Cluau("local f do f = 1 end"), std::string("let f ; { f = 1 }"));
	CHECK(RoundTrips("local f do local a = 1 f = function() return a end end local b = 2\n"));
	// An until condition holding a function value (its own end).
	CHECK(RoundTrips("repeat\n\tcontinue\nuntil function() if x then return end end\n"));
	// A do-block on the line after an until condition is the next statement.
	CHECK(RoundTrips("repeat\n\tf()\nuntil g(x)\ndo end\nrepeat until a and if b then c else d\n"));
	// `\z` and escaped newlines continue a string.
	CHECK(RoundTrips("s = \"a\\z\n   b\"\nt = \"c\\\nd\"\n"));
}

TEST("convert: if-expressions take the cluau form")
{
	CHECK_EQ(Cluau("local x = if a then 1 else 2"), std::string("let x = if (a) 1 else 2"));
	CHECK_EQ(Cluau("local l = if n == 0 then \"none\" elseif n == 1 then \"one\" else \"many\""),
		std::string("let l = if (n == 0) \"none\" else if (n == 1) \"one\" else \"many\""));
	CHECK_EQ(Cluau("return if (a) then f() else g()"), std::string("return if (a) f() else g()"));
	CHECK_EQ(Cluau("x = if a then if b then 1 else 2 else 3"), std::string("x = if (a) if (b) 1 else 2 else 3"));
	CHECK_EQ(Cluau("local v = if a\n\tthen 1\n\telse 2"), std::string("let v = if (a)\n\t 1\n\telse 2"));
	CHECK(RoundTrips("local x = if a then 1 elseif b then 2 else 3\nlocal t = { k = if a then { 1 } else {} }\n"));
	CHECK_EQ(Cluau("x = if if c then false else true then 10 else 20"), std::string("x = if (if (c) false else true) 10 else 20"));
	CHECK(RoundTrips("while if a then b else c do f() end\nif if a then b else c then g() end\n"));
	CHECK(RoundTrips("local f = function() return if a then 1 else 2 end\nif x then y = if z then 1 else 2 end\n"));
}

TEST("convert: a statement after a type, a cast or `...` is a statement")
{
	CHECK_EQ(Cluau("local best: string?\nif x then f() end"), std::string("let best: string?\nif (x) { f() }"));
	CHECK(RoundTrips("local best: string?\nif x then f() end\ntype T = Array<number>\nif x then f() end\n"));
	CHECK(RoundTrips("local m: Map<string, number>, k: number?\nif x then f() end\nlocal v = y :: Array<number>\nif v then f() end\n"));
	CHECK(RoundTrips("local function g(...)\n\tlocal a = ...\n\tif a then f() end\nend\n"));
	// An if-expression after an operator is still one.
	CHECK_EQ(Cluau("local b = a >\n\tif c then 1 else 2"), std::string("let b = a >\n\tif (c) 1 else 2"));
}

TEST("convert: call sugar across a line, a do-block in a function, until's end")
{
	// `f` then `{ ... }` on the next line is a call; in cluau that `{` would be a block.
	CHECK_EQ(Cluau("local r = f\n{ x = 1 }"), std::string("let r = f(\n{ x = 1 })"));
	CHECK(RoundTrips("local r = f\n{ x = 1 }\nlocal s = g \"a\"\n{ 2 }\n"));
	CHECK_EQ(Cluau("local g = function() do return 2 end end"), std::string("let g = function() { { return 2 } }"));
	CHECK(RoundTrips("local g = function(): number do return 2 end end\nfunction h() do end end\n"));
	CHECK_EQ(Cluau("repeat n += 1 until n > 2 print(n)"), std::string("do { n += 1 } while (!(n > 2)); print(n)"));
	CHECK(RoundTrips("repeat n += 1 until n > 2 print(n)\nrepeat until f \"x\" g()\nrepeat until if a then b else c print(1)\n"));
}

TEST("convert: throw and try are reported only where a statement starts")
{
	CHECK(!cluau::Convert("print(throw(1))\nlocal function throw(x) return x end\nlocal t = try { 1 }\n").HasErrors());
	CHECK(HasError(cluau::Convert("throw(1)"), "a call to 'throw'"));
	CHECK(HasError(cluau::Convert("x = 1 throw(1)"), "a call to 'throw'"));
	CHECK(HasError(cluau::Convert("if a then try { 1 } end"), "a call to 'try'"));
}

TEST("convert: an interpolated string's segments convert too")
{
	CHECK_EQ(Cluau("print(`{if (a) then 1 else 2} {a ~= b and not c}`)"), std::string("print(`{if (a) 1 else 2} {a != b && !c}`)"));
	CHECK_EQ(Cluau("s = `x {`y {if x then 1 else 2}`}`"), std::string("s = `x {`y {if (x) 1 else 2}`}`"));
	CHECK(RoundTrips("print(`{if (a) then 1 else 2} {a ~= b and not c} {`{x // 2}`} {\"}\"}`)\nlocal s = `a\\`{1}`\n"));
}

TEST("convert: a long chain of function types is read without recursion")
{
	std::string chain = "local function f(): ";
	for (int i = 0; i < 2000000; i++)
		chain += "() -> ";
	chain += "number end";
	const cluau::ConvertResult r = cluau::Convert(chain);
	CHECK(!r.HasErrors());
	CHECK(r.Cluau.size() > 10 && r.Cluau.substr(r.Cluau.size() - 10) == "number { }");
}
