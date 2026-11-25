#include "test.h"

#include "cluau/runtime.h"

#include "lua.h"
#include "lualib.h"

#include <filesystem>
#include <fstream>

namespace {

	// What scripts hand the host through @test/sink's put().
	std::vector<std::string>& Sunk()
	{
		static std::vector<std::string> values;
		return values;
	}

	int SinkPut(lua_State* L)
	{
		Sunk().push_back(cluau::runtime::Repr(L, 1));
		return 0;
	}

	int OpenSink(lua_State* L)
	{
		lua_newtable(L);
		lua_pushcfunction(L, SinkPut, "put");
		lua_setfield(L, -2, "put");
		return 1;
	}

	int Opened = 0;

	int Twice(lua_State* L)
	{
		lua_pushnumber(L, luaL_checknumber(L, 1) * 2);
		return 1;
	}

	int OpenMath(lua_State* L)
	{
		Opened++;
		lua_newtable(L);
		lua_pushcfunction(L, Twice, "twice");
		lua_setfield(L, -2, "twice");
		return 1;
	}

	int Run(const std::string& source, cluau::runtime::Options options = {})
	{
		Sunk().clear();
		cluau::runtime::Runtime runtime(std::move(options));
		runtime.AddModule("@test/sink", OpenSink);
		runtime.AddModule("@app/math", OpenMath);
		runtime.AddModuleSource("@app/greet", "const math = require(\"@app/math\")\n"
											  "function greet(name: string): string { return `hello, {name} ({math.twice(2)})` }\n"
											  "return { greet = greet }\n");
		return runtime.RunSource(source, "-e");
	}

	std::string Temp(const std::string& name, const std::string& text)
	{
		const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::ofstream(path, std::ios::binary) << text;
		return path.string();
	}

} // namespace

TEST("runtime: a host module made in C++ is required by its alias, loaded once")
{
	Opened = 0;
	CHECK_EQ(Run("const sink = require(\"@test/sink\")\n"
				 "const math = require(\"@app/math\")\n"
				 "sink.put(math.twice(21))\n"
				 "sink.put(require(\"@app/math\") == math)\n"),
		0);
	CHECK_EQ(Sunk().size(), size_t(2));
	if (Sunk().size() == 2)
	{
		CHECK_EQ(Sunk()[0], std::string("42"));
		CHECK_EQ(Sunk()[1], std::string("true"));
	}
	CHECK_EQ(Opened, 1);
}

TEST("runtime: a host module written in cluau requires the host's others")
{
	CHECK_EQ(Run("require(\"@test/sink\").put(require(\"@app/greet\").greet(\"ada\"))\n"), 0);
	CHECK_EQ(Sunk().size(), size_t(1));
	if (!Sunk().empty())
		CHECK_EQ(Sunk()[0], std::string("\"hello, ada (4)\""));
	CHECK_EQ(Run("require(\"@app/missing\")\n"), 1);
}

TEST("runtime: host module names are @alias/name, never @std")
{
	cluau::runtime::Runtime runtime;
	CHECK(runtime.AddModule("@game/world", OpenMath));
	CHECK(!runtime.AddModule("@std/world", OpenMath));
	CHECK(!runtime.AddModule("world", OpenMath));
	CHECK(!runtime.AddModule("@game/", OpenMath));
	CHECK(!runtime.AddModule("@/world", OpenMath));
	CHECK(!runtime.AddModuleSource("@std/x", "return 1"));
}

TEST("runtime: the sandbox has no file, network or process access")
{
	cluau::runtime::Options sandbox;
	sandbox.Sandbox = true;
	sandbox.Args = { "one" };
	CHECK_EQ(Run("const sink = require(\"@test/sink\")\n"
				 "const process = require(\"@std/process\")\n"
				 "sink.put(process.args[1])\n"
				 "sink.put(process.run == nil and process.spawn == nil and process.env == nil and process.chdir == nil)\n"
				 "sink.put(pcall(require, \"@std/fs\"))\n"
				 "sink.put(require(\"@std/json\").encode({ 1, 2 }))\n",
				 sandbox),
		0);
	CHECK_EQ(Sunk().size(), size_t(4));
	if (Sunk().size() == 4)
	{
		CHECK_EQ(Sunk()[0], std::string("\"one\""));
		CHECK_EQ(Sunk()[1], std::string("true"));
		CHECK_EQ(Sunk()[2], std::string("false"));
		CHECK_EQ(Sunk()[3], std::string("\"[1,2]\""));
	}
	CHECK_EQ(Run("require(\"@std/http\")\n", sandbox), 1);
	CHECK_EQ(Run("require(\"@std/fs\")\n"), 0);
}

TEST("runtime: scripts run the same interpreted")
{
	cluau::runtime::Options interpreted;
	interpreted.Native = false;
	CHECK_EQ(Run("let n = 0\nfor (i = 1, 1000) { n += i }\nrequire(\"@test/sink\").put(n)\n", interpreted), 0);
	CHECK_EQ(Sunk().size(), size_t(1));
	if (!Sunk().empty())
		CHECK_EQ(Sunk()[0], std::string("500500"));
}

TEST("check: the host's modules are typed by CheckOptions")
{
	cluau::runtime::CheckOptions options;
	options.Modules["@app/math"] = "local m: any = nil\nreturn m :: { twice: (n: number) -> number }\n";
	options.ModuleSources["@app/greet"] = "function greet(name: string): string { return name }\nreturn { greet = greet }\n";
	const std::string good = Temp("cluau_host_good.cluau", "const math = require(\"@app/math\")\n"
														   "const greet = require(\"@app/greet\")\n"
														   "const n: number = math.twice(2)\n"
														   "const s: string = greet.greet(\"x\")\n");
	const std::string bad = Temp("cluau_host_bad.cluau", "const math = require(\"@app/math\")\n"
														 "const s: string = math.twice(\"x\")\n");
	CHECK(cluau::runtime::Check({ good }, options).empty());
	const std::vector<cluau::runtime::CheckFinding> findings = cluau::runtime::Check({ bad }, options);
	CHECK(!findings.empty());
	for (const cluau::runtime::CheckFinding& f : findings)
		CHECK_EQ(f.At.Begin.Line, 1);
	// Without the types, the alias is unknown.
	CHECK(!cluau::runtime::Check({ good }).empty());
	std::filesystem::remove(good);
	std::filesystem::remove(bad);
}
