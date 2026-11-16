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
	const cluau::ConvertResult ifExpr = cluau::Convert("local x = if a then 1 else 2");
	CHECK(HasError(ifExpr, "if-expression"));
	CHECK_EQ(ifExpr.Cluau, std::string("let x = if a then 1 else 2"));
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
	// `\z` and escaped newlines continue a string.
	CHECK(RoundTrips("s = \"a\\z\n   b\"\nt = \"c\\\nd\"\n"));
}
