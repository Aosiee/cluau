#include "test.h"

#include "cluau/runtime.h"
#include "DapServer.h"
#include "Json.h"
#include "LspServer.h"
#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

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

	// A directory of this run's own (test builds may run side by side),
	// removed at the end.
	const std::filesystem::path& TempDir()
	{
		static const struct Dir
		{
			std::filesystem::path Path;
			Dir()
			{
				std::random_device random;
				Path = std::filesystem::temp_directory_path() / ("cluau_runtime_tests_" + std::to_string(random()) + std::to_string(random()));
				std::filesystem::create_directories(Path);
			}
			~Dir()
			{
				std::error_code ec;
				std::filesystem::remove_all(Path, ec);
			}
		} dir;
		return dir.Path;
	}

	std::string Temp(const std::string& name, const std::string& text)
	{
		const std::filesystem::path path = TempDir() / name;
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

// ---- the language server ---------------------------------------------------


namespace {

	using cluau::runtime::detail::Json;
	using cluau::runtime::detail::LspServer;

	struct Session
	{
		LspServer Server;
		std::vector<Json> Notes; // the notifications the last message raised
		int Id = 0;

		Json Request(const std::string& method, Json params)
		{
			Json message = Json::Object().Set("jsonrpc", "2.0").Set("id", ++Id).Set("method", method).Set("params", std::move(params));
			return Take(Server.Handle(message.Dump()));
		}

		void Notify(const std::string& method, Json params)
		{
			Json message = Json::Object().Set("jsonrpc", "2.0").Set("method", method).Set("params", std::move(params));
			Take(Server.Handle(message.Dump()));
		}

		Json Take(const std::vector<std::string>& out)
		{
			Notes.clear();
			Json response;
			for (const std::string& text : out)
			{
				std::optional<Json> m = Json::Parse(text);
				CHECK(m.has_value());
				if (!m)
					continue;
				if (m->Has("id"))
					response = *m;
				else
					Notes.push_back(*m);
			}
			return response;
		}

		// The diagnostics last published for a file.
		Json Diagnostics(const std::string& uri) const
		{
			for (const Json& note : Notes)
				if (note["method"].String() == "textDocument/publishDiagnostics" && note["params"]["uri"].String() == uri)
					return note["params"]["diagnostics"];
			return {};
		}
	};

	Json At(const std::string& uri, int line, int character)
	{
		return Json::Object()
			.Set("textDocument", Json::Object().Set("uri", uri))
			.Set("position", Json::Object().Set("line", line).Set("character", character));
	}

	Json OpenedDocument(const std::string& uri, const std::string& text)
	{
		return Json::Object().Set(
			"textDocument", Json::Object().Set("uri", uri).Set("languageId", "cluau").Set("version", 1).Set("text", text));
	}

	// A project on disk: main.cluau requires ./shapes.
	struct Project
	{
		std::filesystem::path Dir;
		std::string Main, Shapes;

		Project()
		{
			// Canonical: definitions in files the editor has not opened name
			// them by their canonical paths.
			std::filesystem::create_directories(TempDir() / "lsp");
			Dir = std::filesystem::canonical(TempDir() / "lsp");
			std::ofstream(Dir / "shapes.cluau", std::ios::binary) << "// A point on the plane.\n"
																	 "class Point {\n"
																	 "\tX: number = 0\n"
																	 "\tY: number = 0\n"
																	 "\n"
																	 "\t// How far from the origin.\n"
																	 "\tfunction Length(): number {\n"
																	 "\t\treturn math.sqrt(self.X * self.X + self.Y * self.Y)\n"
																	 "\t}\n"
																	 "}\n"
																	 "\n"
																	 "return { Point = Point }\n";
			Main = LspServer::UriFromPath((Dir / "main.cluau").generic_u8string());
			Shapes = LspServer::UriFromPath((Dir / "shapes.cluau").generic_u8string());
		}
		~Project()
		{
			std::error_code ec;
			std::filesystem::remove_all(Dir, ec);
		}
	};

	const char* kMain = "const fs = require(\"@std/fs\")\n"
						"const shapes = require(\"./shapes\")\n"
						"\n"
						"// The number of lines in a file.\n"
						"function count(path: string, limit: number): number {\n"
						"\treturn #string.split(fs.read(path), \"\\n\")\n"
						"}\n"
						"\n"
						"const p = shapes.Point()\n"
						"print(p:Length(), count(\"x\", 3))\n"
						"let total = count(\"main.cluau\", 1) + 1\n"
						"print(total, missing)\n";

} // namespace

TEST("json: values read back as they were written")
{
	std::optional<Json> v = Json::Parse(R"({"a": [1, 2.5, "x\"y", true, null], "b": {"c": "\u00e9\ud83d\ude00"}, "n": -3e2})");
	CHECK(v.has_value());
	if (!v)
		return;
	CHECK_EQ((*v)["a"].Size(), size_t(5));
	CHECK_EQ((*v)["a"][1].Number(), 2.5);
	CHECK_EQ((*v)["a"][2].String(), std::string("x\"y"));
	CHECK((*v)["a"][4].IsNull());
	CHECK_EQ((*v)["b"]["c"].String(), std::string("\xc3\xa9\xf0\x9f\x98\x80"));
	CHECK_EQ((*v)["n"].Int(), -300);
	CHECK((*v)["missing"]["deeper"].IsNull());
	CHECK_EQ(Json::Parse(v->Dump())->Dump(), v->Dump());
	CHECK_EQ(Json::Object().Set("k", "a\nb").Set("n", 3).Dump(), std::string(R"({"k":"a\nb","n":3})"));
	CHECK(!Json::Parse("{\"a\": }").has_value());
	CHECK(!Json::Parse("[1, 2").has_value());
}

TEST("lsp: an opened script's problems are published at their lines")
{
	Project project;
	Session s;
	const Json init = s.Request("initialize", Json::Object().Set("capabilities", Json::Object()));
	CHECK(init["result"]["capabilities"]["hoverProvider"].Bool());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, kMain));
	const Json diagnostics = s.Diagnostics(project.Main);
	CHECK_EQ(diagnostics.Size(), size_t(1));
	CHECK_EQ(diagnostics[0]["range"]["start"]["line"].Int(), 11);
	CHECK_EQ(diagnostics[0]["range"]["start"]["character"].Int(), 13);
	CHECK_EQ(diagnostics[0]["range"]["end"]["character"].Int(), 20);
	CHECK(diagnostics[0]["message"].String().find("missing") != std::string::npos);
	// Fixed in the buffer: nothing to say.
	std::string fixed = kMain;
	fixed.replace(fixed.find("missing"), 7, "total");
	s.Notify("textDocument/didChange", Json::Object()
										   .Set("textDocument", Json::Object().Set("uri", project.Main))
										   .Set("contentChanges", Json::Array().Push(Json::Object().Set("text", fixed))));
	CHECK_EQ(s.Diagnostics(project.Main).Size(), size_t(0));
	CHECK(s.Diagnostics(project.Main).IsArray());
	// Closed: its diagnostics are cleared.
	s.Notify("textDocument/didClose", Json::Object().Set("textDocument", Json::Object().Set("uri", project.Main)));
	CHECK(s.Diagnostics(project.Main).IsArray());
	CHECK_EQ(s.Diagnostics(project.Main).Size(), size_t(0));
}

TEST("lsp: completion, hover and signature help know the script's types")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	std::string text = kMain;
	text += "print(fs.re)\nprint(count(\"a\", \n";
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, text));

	auto labels = [](const Json& items) {
		std::vector<std::string> out;
		for (const Json& item : items.Items())
			out.push_back(item["label"].String());
		return out;
	};
	const std::vector<std::string> members = labels(s.Request("textDocument/completion", At(project.Main, 12, 11))["result"]);
	CHECK(std::find(members.begin(), members.end(), "read") != members.end());
	CHECK(std::find(members.begin(), members.end(), "readBuffer") != members.end());
	const std::vector<std::string> methods = labels(s.Request("textDocument/completion", At(project.Main, 9, 8))["result"]);
	CHECK(std::find(methods.begin(), methods.end(), "Length") != methods.end());

	auto hover = [&](int line, int character) {
		return s.Request("textDocument/hover", At(project.Main, line, character))["result"]["contents"]["value"].String();
	};
	CHECK_EQ(hover(10, 6), std::string("```luau\ntotal: number\n```"));
	CHECK_EQ(hover(9, 6), std::string("```luau\np: Point\n```"));
	CHECK_EQ(hover(9, 10), std::string("```luau\nPoint:Length(): number\n```\n\nHow far from the origin."));
	CHECK_EQ(hover(8, 19), std::string("```luau\nclass Point\n```\n\nA point on the plane."));
	CHECK_EQ(hover(9, 20), std::string("```luau\ncount: (path: string, limit: number) -> number\n```\n\nThe number of lines in a file."));

	const Json signature = s.Request("textDocument/signatureHelp", At(project.Main, 13, 17))["result"];
	CHECK_EQ(signature["signatures"][0]["label"].String(), std::string("count(path: string, limit: number): number"));
	CHECK_EQ(signature["activeParameter"].Int(), 1);
}

TEST("lsp: definitions, references, renaming, the outline and formatting")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, kMain));

	const Json method = s.Request("textDocument/definition", At(project.Main, 9, 10))["result"];
	CHECK_EQ(method["uri"].String(), project.Shapes);
	CHECK_EQ(method["range"]["start"]["line"].Int(), 6);
	CHECK_EQ(method["range"]["start"]["character"].Int(), 10);
	const Json function = s.Request("textDocument/definition", At(project.Main, 9, 19))["result"];
	CHECK_EQ(function["uri"].String(), project.Main);
	CHECK_EQ(function["range"]["start"]["line"].Int(), 4);
	CHECK_EQ(function["range"]["start"]["character"].Int(), 9);
	const Json module = s.Request("textDocument/definition", At(project.Main, 1, 26))["result"];
	CHECK_EQ(module["uri"].String(), project.Shapes);

	Json refs = At(project.Main, 11, 7);
	refs.Set("context", Json::Object().Set("includeDeclaration", true));
	const Json uses = s.Request("textDocument/references", refs)["result"];
	CHECK_EQ(uses.Size(), size_t(2));
	CHECK_EQ(uses[0]["range"]["start"]["line"].Int(), 10);
	CHECK_EQ(uses[0]["range"]["start"]["character"].Int(), 4);

	Json rename = At(project.Main, 10, 5);
	rename.Set("newName", "sum");
	const Json edits = s.Request("textDocument/rename", rename)["result"]["changes"][project.Main];
	CHECK_EQ(edits.Size(), size_t(2));
	CHECK_EQ(edits[1]["newText"].String(), std::string("sum"));
	rename.Set("newName", "while");
	CHECK(s.Request("textDocument/rename", rename).Has("error"));

	const Json outline = s.Request("textDocument/documentSymbol", Json::Object().Set("textDocument", Json::Object().Set("uri", project.Main)))["result"];
	std::vector<std::string> names;
	for (const Json& symbol : outline.Items())
		names.push_back(symbol["name"].String());
	std::string joined;
	for (const std::string& n : names)
		joined += n + " ";
	CHECK_EQ(joined, std::string("fs shapes count p total "));
	s.Notify("textDocument/didOpen", OpenedDocument(project.Shapes, "class Point {\n\tX: number = 0\n\tfunction Length(): number { return 0 }\n}\nreturn { Point = Point }\n"));
	const Json classes = s.Request("textDocument/documentSymbol", Json::Object().Set("textDocument", Json::Object().Set("uri", project.Shapes)))["result"];
	CHECK_EQ(classes[0]["name"].String(), std::string("Point"));
	CHECK_EQ(classes[0]["children"].Size(), size_t(2));

	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "if (x) {\ny()\n}\n"));
	const Json format = s.Request("textDocument/formatting", Json::Object()
															  .Set("textDocument", Json::Object().Set("uri", project.Main))
															  .Set("options", Json::Object().Set("tabSize", 2).Set("insertSpaces", true)))["result"];
	CHECK_EQ(format.Size(), size_t(1));
	CHECK_EQ(format[0]["newText"].String(), std::string("if (x) {\n  y()\n}\n"));
}

TEST("lsp: a member's references and renames reach every file of the workspace")
{
	Project project;
	std::ofstream(project.Dir / "other.cluau", std::ios::binary) << "const shapes = require(\"./shapes\")\n"
																	"print(shapes.Point():Length())\n";
	const std::string other = LspServer::UriFromPath((project.Dir / "other.cluau").generic_u8string());
	Session s;
	s.Request("initialize", Json::Object().Set("rootUri", LspServer::UriFromPath(project.Dir.generic_u8string())));
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, kMain));

	// p:Length() in main.cluau: the method's declaration, and its uses in
	// main.cluau and in other.cluau (which is not open).
	Json refs = At(project.Main, 9, 10);
	refs.Set("context", Json::Object().Set("includeDeclaration", true));
	const Json uses = s.Request("textDocument/references", refs)["result"];
	std::set<std::string> found;
	for (const Json& use : uses.Items())
		found.insert(use["uri"].String() + ":" + std::to_string(use["range"]["start"]["line"].Int()) + ":" +
			std::to_string(use["range"]["start"]["character"].Int()));
	CHECK_EQ(uses.Size(), size_t(3));
	CHECK(found.count(project.Shapes + ":6:10"));
	CHECK(found.count(project.Main + ":9:8"));
	CHECK(found.count(other + ":1:21"));
	CHECK_EQ(uses[0]["uri"].String(), project.Shapes); // the declaration first

	// A field, from a use inside its class: the declaration and both uses on the line.
	Json field = At(project.Shapes, 7, 24);
	field.Set("context", Json::Object().Set("includeDeclaration", true));
	const Json fieldUses = s.Request("textDocument/references", field)["result"];
	CHECK_EQ(fieldUses.Size(), size_t(3));
	CHECK_EQ(fieldUses[0]["range"]["start"]["line"].Int(), 2);

	Json rename = At(project.Main, 9, 10);
	rename.Set("newName", "Size");
	const Json changes = s.Request("textDocument/rename", rename)["result"]["changes"];
	CHECK_EQ(changes.Size(), size_t(3));
	CHECK_EQ(changes[other].Size(), size_t(1));
	// A standard module's member is not the workspace's to rename.
	Json stdRename = At(project.Main, 5, 25);
	stdRename.Set("newName", "load");
	CHECK(s.Request("textDocument/rename", stdRename).Has("error"));
}

TEST("lsp: workspace symbols, and inlay hints for the types of names declared without one")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object().Set("workspaceFolders",
								Json::Array().Push(Json::Object().Set("uri", LspServer::UriFromPath(project.Dir.generic_u8string())).Set("name", "lsp"))));
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, kMain));

	const Json length = s.Request("workspace/symbol", Json::Object().Set("query", "len"))["result"];
	bool method = false;
	for (const Json& symbol : length.Items())
		method = method || (symbol["name"].String() == "Length" && symbol["containerName"].String() == "Point" &&
							   symbol["location"]["uri"].String() == project.Shapes);
	CHECK(method);
	const Json count = s.Request("workspace/symbol", Json::Object().Set("query", "cnt"))["result"];
	CHECK_EQ(count.Size(), size_t(1));
	CHECK_EQ(count[0]["name"].String(), std::string("count"));

	const Json hints = s.Request("textDocument/inlayHint",
		Json::Object()
			.Set("textDocument", Json::Object().Set("uri", project.Main))
			.Set("range", Json::Object()
							  .Set("start", Json::Object().Set("line", 0).Set("character", 0))
							  .Set("end", Json::Object().Set("line", 12).Set("character", 0))))["result"];
	std::map<std::string, std::string> labels;
	for (const Json& hint : hints.Items())
		labels[std::to_string(hint["position"]["line"].Int()) + ":" + std::to_string(hint["position"]["character"].Int())] = hint["label"].String();
	CHECK_EQ(labels["8:7"], std::string(": Point"));
	CHECK_EQ(labels["10:9"], std::string(": number"));
}

TEST("coverage: each line with code, and the times it ran")
{
	const std::string file = Temp("covered.cluau", "function half(n: number): number {\n" // 1
												   "\tif (n < 0) {\n"                    // 2
												   "\t\terror(\"negative\")\n"          // 3
												   "\t}\n"                               // 4
												   "\treturn n / 2\n"                    // 5
												   "}\n"                                  // 6
												   "for (i = 1, 3) {\n"                   // 7
												   "\thalf(i)\n"                         // 8
												   "}\n");                                // 9
	cluau::runtime::Options options;
	options.Coverage = true;
	cluau::runtime::Runtime runtime(options);
	CHECK_EQ(runtime.RunFile(file), 0);
	const auto coverage = runtime.Coverage();
	CHECK_EQ(coverage.size(), size_t(1));
	if (coverage.empty())
		return;
	const std::map<int, int>& lines = coverage.begin()->second;
	CHECK(lines.count(3) && lines.at(3) == 0); // the error never ran
	CHECK(lines.count(5) && lines.at(5) == 3);
	CHECK(lines.count(8) && lines.at(8) == 3);
	CHECK(!lines.count(4) && !lines.count(6)); // no code
}

TEST("profile: samples name the functions the time went to, as folded stacks")
{
	const std::string file = Temp("busy.cluau", "function spin(): number {\n"
												"\tlet total = 0\n"
												"\tconst stop = os.clock() + 0.15\n"
												"\twhile (os.clock() < stop) {\n"
												"\t\ttotal += 1\n"
												"\t}\n"
												"\treturn total\n"
												"}\n"
												"spin()\n");
	const std::filesystem::path folded = TempDir() / "busy.folded";
	{
		cluau::runtime::Options options;
		options.Profile = true;
		options.ProfileFolded = folded.u8string();
		cluau::runtime::Runtime runtime(options);
		CHECK_EQ(runtime.RunFile(file), 0);
	}
	std::ifstream in(folded, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	CHECK(text.str().find("(top level) (") != std::string::npos);
	CHECK(text.str().find(";spin (") != std::string::npos);
}

TEST("profile: a module's top level by its name; a wait in @std/task goes to the line that waits")
{
	Temp("heavy.cluau", "let s = 0\n"
						"const stop = os.clock() + 0.1\n"
						"while (os.clock() < stop) {\n"
						"\ts += 1\n"
						"}\n"
						"return s\n");
	const std::string file = Temp("waits.cluau", "const task = require(\"@std/task\")\n"
												 "const heavy = require(\"./heavy\")\n"
												 "task.wait(0.2)\n"
												 "print(heavy > 0)\n");
	const std::filesystem::path folded = TempDir() / "waits.folded";
	{
		cluau::runtime::Options options;
		options.Profile = true;
		options.ProfileFolded = folded.u8string();
		cluau::runtime::Runtime runtime(options);
		CHECK_EQ(runtime.RunFile(file), 0);
	}
	std::ifstream in(folded, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	CHECK(text.str().find(";(top level) (") != std::string::npos);       // heavy.cluau's, under the script's
	CHECK(text.str().find("heavy.cluau:1)") == std::string::npos);       // not an anonymous function
	CHECK(text.str().find("@std/task") == std::string::npos);            // the wait is the script's line's
}

TEST("lsp: quick fixes for a misspelt name, a missing standard module, and any")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "const shapes = require(\"./shapes\")\n" // 0
																 "const p = shapes.Point()\n"             // 1
																 "print(p:Lenght())\n"                    // 2
																 "prnt(1)\n"                              // 3
																 "print(json.encode({ a = 1 }))\n"        // 4
																 "const loose: any = 1\n"));              // 5
	const Json diagnostics = s.Diagnostics(project.Main);
	CHECK(diagnostics.Size() >= 4);
	const Json actions = s.Request("textDocument/codeAction",
		Json::Object()
			.Set("textDocument", Json::Object().Set("uri", project.Main))
			.Set("range", diagnostics[0]["range"])
			.Set("context", Json::Object().Set("diagnostics", diagnostics)))["result"];
	std::map<std::string, Json> byTitle;
	for (const Json& action : actions.Items())
		byTitle[action["title"].String()] = action;
	CHECK(byTitle.count("Change to 'Length'"));
	CHECK(byTitle.count("Change to 'print'"));
	CHECK(byTitle.count("Add const json = require(\"@std/json\")"));
	CHECK(byTitle.count("Allow any on this line (// allow-any:)"));
	if (byTitle.count("Change to 'Length'"))
	{
		const Json edit = byTitle["Change to 'Length'"]["edit"]["changes"][project.Main][0];
		CHECK_EQ(edit["range"]["start"]["line"].Int(), 2);
		CHECK_EQ(edit["range"]["start"]["character"].Int(), 8);
		CHECK_EQ(edit["newText"].String(), std::string("Length"));
	}
	if (byTitle.count("Add const json = require(\"@std/json\")"))
	{
		const Json edit = byTitle["Add const json = require(\"@std/json\")"]["edit"]["changes"][project.Main][0];
		CHECK_EQ(edit["range"]["start"]["line"].Int(), 1); // after the file's requires
	}
}

TEST("lsp: the any exemption on a line with a comment is one cluau check honours")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	const std::string line = "const loose: any = \"// not a comment\" // the value";
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, line + "\nprint(loose)\n"));
	Json noAny;
	const Json diagnostics = s.Diagnostics(project.Main);
	for (const Json& d : diagnostics.Items())
		if (d["code"].String() == "NoAny" && d["range"]["start"]["line"].Int() == 0)
			noAny = d;
	CHECK(noAny.IsObject());
	const Json actions = s.Request("textDocument/codeAction", Json::Object()
																 .Set("textDocument", Json::Object().Set("uri", project.Main))
																 .Set("range", noAny["range"])
																 .Set("context", Json::Object().Set("diagnostics", Json::Array().Push(noAny))))["result"];
	CHECK_EQ(actions.Size(), size_t(1));
	if (actions.Size() != 1)
		return;
	const Json edit = actions[0]["edit"]["changes"][project.Main][0];
	CHECK_EQ(edit["range"]["start"]["line"].Int(), 0);
	std::string fixed = line;
	const int from = edit["range"]["start"]["character"].Int(), to = edit["range"]["end"]["character"].Int();
	fixed.replace((size_t)from, (size_t)(to - from), edit["newText"].String());
	CHECK_EQ(fixed, std::string("const loose: any = \"// not a comment\" // allow-any: reason - the value"));
	const std::filesystem::path file = project.Dir / "exempt.cluau";
	std::ofstream(file, std::ios::binary) << fixed << "\n";
	for (const cluau::runtime::CheckFinding& f : cluau::runtime::Check({ file.u8string() }))
		CHECK(f.At.Begin.Line != 0);
}

TEST("lsp: semantic tokens name each name's kind")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, kMain));
	const Json data = s.Request("textDocument/semanticTokens/full", Json::Object().Set("textDocument", Json::Object().Set("uri", project.Main)))["result"]["data"];
	CHECK(data.Size() % 5 == 0 && data.Size() > 0);
	// line:col -> type index, modifiers
	std::map<std::string, std::pair<int, int>> tokens;
	int line = 0, col = 0;
	for (size_t i = 0; i + 4 < data.Size(); i += 5)
	{
		const int dl = data[i].Int(), dc = data[i + 1].Int();
		line += dl;
		col = dl ? dc : col + dc;
		tokens[std::to_string(line) + ":" + std::to_string(col)] = { data[i + 3].Int(), data[i + 4].Int() };
	}
	enum { Namespace, Class, Enum, Type, Parameter, Variable, Property, Function, Method };
	CHECK(tokens.count("4:9") && tokens["4:9"].first == Function);            // count
	CHECK(tokens.count("4:15") && tokens["4:15"].first == Parameter);         // path
	CHECK(tokens.count("4:15") && (tokens["4:15"].second & 1));               // ... declared there
	CHECK(tokens.count("5:9") && tokens["5:9"].first == Namespace);           // string
	CHECK(tokens.count("5:9") && (tokens["5:9"].second & 4));                 // ... the standard library's
	CHECK(tokens.count("5:16") && tokens["5:16"].first == Method);            // split
	CHECK(tokens.count("8:6") && tokens["8:6"].first == Variable);            // p
	CHECK(tokens.count("9:8") && tokens["9:8"].first == Method);              // Length
}

TEST("lsp: the call hierarchy: what calls a function, and what it calls")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "function inner(): number {\n"   // 0
																 "\treturn 1\n"                   // 1
																 "}\n"                             // 2
																 "function outer(): number {\n"    // 3
																 "\treturn inner() + inner()\n"   // 4
																 "}\n"                             // 5
																 "print(outer(), inner())\n"));    // 6
	const Json prepared = s.Request("textDocument/prepareCallHierarchy", At(project.Main, 6, 15))["result"];
	CHECK_EQ(prepared.Size(), size_t(1));
	if (prepared.Size() != 1)
		return;
	CHECK_EQ(prepared[0]["name"].String(), std::string("inner"));
	CHECK_EQ(prepared[0]["range"]["start"]["line"].Int(), 0);
	const Json incoming = s.Request("callHierarchy/incomingCalls", Json::Object().Set("item", prepared[0]))["result"];
	std::map<std::string, size_t> callers;
	for (const Json& call : incoming.Items())
		callers[call["from"]["name"].String()] = call["fromRanges"].Size();
	CHECK_EQ(callers["outer"], size_t(2));
	CHECK_EQ(callers["(top level)"], size_t(1));

	const Json outer = s.Request("textDocument/prepareCallHierarchy", At(project.Main, 3, 10))["result"];
	CHECK_EQ(outer.Size(), size_t(1));
	if (outer.Size() != 1)
		return;
	const Json outgoing = s.Request("callHierarchy/outgoingCalls", Json::Object().Set("item", outer[0]))["result"];
	CHECK_EQ(outgoing.Size(), size_t(1));
	CHECK_EQ(outgoing[0]["to"]["name"].String(), std::string("inner"));
	CHECK_EQ(outgoing[0]["fromRanges"].Size(), size_t(2));
}

TEST("lsp: columns are UTF-16 code units, and messages are framed on the wire")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	// "é" is two bytes, one UTF-16 unit; "😀" four bytes, two units.
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "const s = \"\xc3\xa9\xf0\x9f\x98\x80\"; print(s, nope)\n"));
	const Json diagnostics = s.Diagnostics(project.Main);
	CHECK_EQ(diagnostics.Size(), size_t(1));
	CHECK_EQ(diagnostics[0]["range"]["start"]["character"].Int(), 26); // 29 in bytes

	std::FILE* in = std::tmpfile();
	std::FILE* out = std::tmpfile();
	CHECK(in && out);
	if (!in || !out)
		return;
	for (const std::string& body : { std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"),
			 std::string(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})"), std::string(R"({"jsonrpc":"2.0","method":"exit"})") })
		std::fprintf(in, "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
	std::rewind(in);
	LspServer server;
	CHECK_EQ(server.Serve(in, out), 0);
	std::rewind(out);
	std::string written;
	for (int c; (c = std::fgetc(out)) != EOF;)
		written += (char)c;
	CHECK(written.rfind("Content-Length: ", 0) == 0);
	CHECK(written.find("\"serverInfo\"") != std::string::npos);
	CHECK(written.find("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}") != std::string::npos);
	std::fclose(in);
	std::fclose(out);
}

TEST("lsp: a module checked again leaves no stale class types behind (a closed module's)")
{
	Project project;
	std::filesystem::create_directories(project.Dir / "lib");
	const std::string vehicleText = "class Vehicle {\n"
									"\tWheels: number = 4\n"
									"\n"
									"\tfunction Init(wheels: number) {\n"
									"\t\tself.Wheels = wheels\n"
									"\t}\n"
									"}\n";
	const std::string bikeText = "const Vehicle = require(\"./lib/vehicle\")\n"
								 "class Bike extends Vehicle {\n"
								 "}\n"
								 "print(Bike(2))\n";
	std::ofstream(project.Dir / "lib" / "vehicle.cluau", std::ios::binary) << vehicleText;
	std::ofstream(project.Dir / "bike.cluau", std::ios::binary) << bikeText;
	const std::string vehicle = LspServer::UriFromPath((project.Dir / "lib" / "vehicle.cluau").generic_u8string());
	const std::string bike = LspServer::UriFromPath((project.Dir / "bike.cluau").generic_u8string());
	Session s;
	s.Request("initialize", Json::Object().Set("rootUri", LspServer::UriFromPath(project.Dir.generic_u8string())));
	s.Notify("textDocument/didOpen", OpenedDocument(bike, bikeText));
	s.Notify("textDocument/didOpen", OpenedDocument(vehicle, vehicleText));
	s.Notify("textDocument/didClose", Json::Object().Set("textDocument", Json::Object().Set("uri", bike)));
	// The closed file's module is not checked again: its types point into
	// the vehicle module's old ones.
	for (int i = 1; i <= 3; i++)
	{
		s.Notify("textDocument/didChange", Json::Object()
												.Set("textDocument", Json::Object().Set("uri", vehicle).Set("version", i + 1))
												.Set("contentChanges", Json::Array().Push(Json::Object().Set("text", vehicleText + std::string((size_t)i, '\n')))));
		CHECK_EQ(s.Diagnostics(vehicle).Size(), size_t(0));
		CHECK(s.Diagnostics(vehicle).IsArray());
	}
	const Json hover = s.Request("textDocument/hover", At(vehicle, 0, 7))["result"];
	CHECK(hover["contents"]["value"].String().find("Vehicle") != std::string::npos);
}

TEST("lsp: a require of a file that is not there resolves once the file appears")
{
	Project project;
	const std::filesystem::path later = project.Dir / "later.cluau";
	std::filesystem::remove(later);
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "const later = require(\"./later\")\nprint(later.X)\n"));
	Json diagnostics = s.Diagnostics(project.Main);
	CHECK(diagnostics.Size() >= 1 && diagnostics[0]["message"].String().find("not found") != std::string::npos);
	std::ofstream(later, std::ios::binary) << "return { X = 1 }\n";
	const std::string uri = LspServer::UriFromPath(later.generic_u8string());
	s.Notify("workspace/didChangeWatchedFiles", Json::Object().Set("changes", Json::Array().Push(Json::Object().Set("uri", uri).Set("type", 1))));
	CHECK_EQ(s.Diagnostics(project.Main).Size(), size_t(0));
	// And the other way: deleted, the require names no file again.
	std::filesystem::remove(later);
	s.Notify("workspace/didChangeWatchedFiles", Json::Object().Set("changes", Json::Array().Push(Json::Object().Set("uri", uri).Set("type", 3))));
	diagnostics = s.Diagnostics(project.Main);
	CHECK(diagnostics.Size() >= 1 && diagnostics[0]["message"].String().find("not found") != std::string::npos);
}

TEST("lsp: the runtime's globals are not renamed; a function's references without its declaration")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "function A(): number {\n" // 0
																 "\treturn B()\n"            // 1
																 "}\n"                        // 2
																 "function B(): number {\n"   // 3
																 "\treturn 1\n"               // 4
																 "}\n"                        // 5
																 "print(A())\n"               // 6
																 "print(B())\n"));            // 7
	const Json refused = s.Request("textDocument/rename", At(project.Main, 6, 2).Set("newName", "say"));
	CHECK(refused.Has("error"));
	CHECK(refused["error"]["message"].String().find("'print'") != std::string::npos);
	// Its uses are still listed, all of them (it has no declaration here).
	const Json uses =
		s.Request("textDocument/references", At(project.Main, 6, 2).Set("context", Json::Object().Set("includeDeclaration", false)))["result"];
	CHECK_EQ(uses.Size(), size_t(2));

	auto lines = [&](bool declaration) {
		std::vector<int> out;
		const Json found =
			s.Request("textDocument/references", At(project.Main, 3, 9).Set("context", Json::Object().Set("includeDeclaration", declaration)))["result"];
		for (const Json& use : found.Items())
			out.push_back(use["range"]["start"]["line"].Int());
		std::sort(out.begin(), out.end());
		return out;
	};
	CHECK(lines(true) == std::vector<int>({ 1, 3, 7 }));
	CHECK(lines(false) == std::vector<int>({ 1, 7 })); // the declaration (line 3) left out, not the first use
	const Json renamed = s.Request("textDocument/rename", At(project.Main, 1, 8).Set("newName", "C"))["result"];
	CHECK_EQ(renamed["changes"][project.Main].Size(), size_t(3));
}

TEST("lsp: semantic tokens for the uses of locals and parameters, quickly for a long file")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "function Add(a: number, b: number): number {\n" // 0
																 "\treturn a + b\n"                               // 1
																 "}\n"                                             // 2
																 "let x = 1\n"                                     // 3
																 "print(Add(x, x))\n"));                           // 4
	auto tokens = [&](const std::string& uri) {
		const Json data = s.Request("textDocument/semanticTokens/full", Json::Object().Set("textDocument", Json::Object().Set("uri", uri)))["result"]["data"];
		std::map<std::pair<int, int>, std::pair<int, int>> out; // (line, col) -> type, modifiers
		int line = 0, col = 0;
		for (size_t i = 0; i + 4 < data.Size(); i += 5)
		{
			const int dl = data[i].Int(), dc = data[i + 1].Int();
			line += dl;
			col = dl ? dc : col + dc;
			out[{ line, col }] = { data[i + 3].Int(), data[i + 4].Int() };
		}
		return out;
	};
	enum { Namespace, Class, Enum, Type, Parameter, Variable, Property, Function, Method };
	auto found = tokens(project.Main);
	// The token at a place: its type and modifiers (-1 for none there).
	auto at = [&](int line, int col) {
		auto it = found.find(std::make_pair(line, col));
		return it == found.end() ? std::make_pair(-1, -1) : it->second;
	};
	CHECK(at(1, 8) == std::make_pair((int)Parameter, 0));  // a
	CHECK(at(1, 12) == std::make_pair((int)Parameter, 0)); // b
	CHECK(at(4, 10).first == Variable);                    // x
	CHECK(at(4, 13).first == Variable);                    // x
	CHECK(at(0, 13).second == 1);                          // a, declared

	// Each position is converted through its line, without reading the
	// file from its start each time.
	std::string big;
	for (int i = 0; i < 20000; i++)
		big += "let v" + std::to_string(i) + " = " + std::to_string(i) + "\nprint(v" + std::to_string(i) + ")\n";
	const std::string uri = LspServer::UriFromPath((project.Dir / "big.cluau").generic_u8string());
	s.Notify("textDocument/didOpen", OpenedDocument(uri, big));
	const auto start = std::chrono::steady_clock::now();
	found = tokens(uri);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	CHECK(found.size() >= 60000);
	CHECK(seconds < 5);
}

TEST("lsp: protocol errors are answered: text that is not JSON, a message too long to take")
{
	Session s;
	std::vector<std::string> out = s.Server.Handle("{not json");
	CHECK_EQ(out.size(), size_t(1));
	if (!out.empty())
	{
		const std::optional<Json> error = Json::Parse(out[0]);
		CHECK(error && (*error)["error"]["code"].Int() == -32700 && (*error)["id"].IsNull());
	}
	out = s.Server.Handle(R"({"jsonrpc":"2.0","id":4})");
	CHECK(out.size() == 1 && Json::Parse(out[0]).value_or(Json())["error"]["code"].Int() == -32600);
	CHECK(s.Server.Handle(R"({"jsonrpc":"2.0","id":4,"result":null})").empty()); // a client's response
	// A number past int's range is read as its end, not cast.
	CHECK_EQ(Json::Parse("1e300")->Int(), 2147483647);
	CHECK_EQ(Json::Parse("-1e300")->Int(), -2147483647 - 1);
	Project project;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "let  x = 1\n"));
	const Json formatted = s.Request("textDocument/formatting", Json::Object()
																 .Set("textDocument", Json::Object().Set("uri", project.Main))
																 .Set("options", Json::Object().Set("insertSpaces", true).Set("tabSize", 1e12)));
	CHECK(formatted["result"].IsArray());

	std::FILE* in = std::tmpfile();
	std::FILE* written = std::tmpfile();
	CHECK(in && written);
	if (!in || !written)
		return;
	std::fprintf(in, "Content-Length: 99999999999999\r\n\r\n");
	std::rewind(in);
	LspServer server;
	CHECK_EQ(server.Serve(in, written), 1);
	std::rewind(written);
	std::string text;
	for (int c; (c = std::fgetc(written)) != EOF;)
		text += (char)c;
	CHECK(text.find("\"code\":-32600") != std::string::npos);
	std::fclose(in);
	std::fclose(written);
}

TEST("lsp: each standard module used without its require has the quick fix that adds it")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	const std::string all = cluau::runtime::detail::StdNames();
	int modules = 0;
	for (size_t at = 0; (at = all.find("@std/", at)) != std::string::npos; modules++)
	{
		at += 5;
		const std::string module = all.substr(at, all.find(',', at) - at);
		s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "print(" + module + ")\n"));
		const Json diagnostics = s.Diagnostics(project.Main);
		const Json actions = s.Request("textDocument/codeAction", Json::Object()
																	 .Set("textDocument", Json::Object().Set("uri", project.Main))
																	 .Set("range", diagnostics[0]["range"])
																	 .Set("context", Json::Object().Set("diagnostics", diagnostics)))["result"];
		bool found = false;
		for (const Json& action : actions.Items())
			found = found || action["title"].String() == "Add const " + module + " = require(\"@std/" + module + "\")";
		if (!(found))
			cluau_test::Fail(__FILE__, __LINE__, module);
		s.Notify("textDocument/didClose", Json::Object().Set("textDocument", Json::Object().Set("uri", project.Main)));
	}
	CHECK(modules >= 20);
}

TEST("lsp: hover on a declaration's name shows what a use of it shows; no hints on class and enum names")
{
	Project project;
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "// Adds one.\n"                      // 0
																 "function AddOne(x: number): number {\n" // 1
																 "\treturn x + 1\n"                       // 2
																 "}\n"                                    // 3
																 "const M = {}\n"                         // 4
																 "// A member.\n"                         // 5
																 "function M.Get(k: string): string {\n"  // 6
																 "\treturn k\n"                           // 7
																 "}\n"                                    // 8
																 "class Box {\n"                          // 9
																 "\t// The width.\n"                      // 10
																 "\tW: number = 1\n"                      // 11
																 "\t// Grows it.\n"                       // 12
																 "\tfunction Grow(by: number) {\n"        // 13
																 "\t\tself.W += by\n"                     // 14
																 "\t}\n"                                  // 15
																 "}\n"                                    // 16
																 "enum Color { Red, Green }\n"            // 17
																 "for (k, v in Color) { print(k, v) }\n"  // 18
																 "print(AddOne(1), M.Get(\"k\"), Box(), Color.Red)\n"));
	auto hover = [&](int line, int character) {
		return s.Request("textDocument/hover", At(project.Main, line, character))["result"]["contents"]["value"].String();
	};
	CHECK(hover(1, 10).find("AddOne: (x: number) -> number") != std::string::npos);
	CHECK(hover(1, 10).find("Adds one.") != std::string::npos);
	CHECK(hover(6, 12).find("Get: (k: string) -> string") != std::string::npos);
	CHECK(hover(6, 12).find("A member.") != std::string::npos);
	CHECK(hover(11, 1).find("W: number") != std::string::npos);
	CHECK(hover(11, 1).find("The width.") != std::string::npos);
	CHECK(hover(13, 11).find("Box:Grow(by: number)") != std::string::npos);
	CHECK(hover(13, 11).find("Grows it.") != std::string::npos);
	CHECK(hover(9, 7).find("class Box") != std::string::npos);
	CHECK(hover(17, 13).find("Red: \"Red\"") != std::string::npos);

	const Json hints = s.Request("textDocument/inlayHint", Json::Object()
															  .Set("textDocument", Json::Object().Set("uri", project.Main))
															  .Set("range", Json::Object()
																				.Set("start", Json::Object().Set("line", 0).Set("character", 0))
																				.Set("end", Json::Object().Set("line", 20).Set("character", 0))))["result"];
	for (const Json& hint : hints.Items())
	{
		const int line = hint["position"]["line"].Int();
		if (!(line != 9 && line != 17))
			cluau_test::Fail(__FILE__, __LINE__, hint.Dump());
		if (!(hint["label"].String() != ": unknown"))
			cluau_test::Fail(__FILE__, __LINE__, hint.Dump());
	}
}

TEST("lsp: the paths a require can name are completed")
{
	Project project;
	std::filesystem::create_directories(project.Dir / "lib");
	Session s;
	s.Request("initialize", Json::Object());
	s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "const a = require(\"@std/f\")\n"
																 "const b = require(\"./\")\n"));
	std::set<std::string> labels;
	const Json std = s.Request("textDocument/completion", At(project.Main, 0, 25));
	for (const Json& item : std["result"].Items())
		labels.insert(item["label"].String());
	CHECK(labels.count("@std/fs"));
	CHECK(!labels.count("@std/json"));
	labels.clear();
	const Json files = s.Request("textDocument/completion", At(project.Main, 1, 21));
	for (const Json& item : files["result"].Items())
		labels.insert(item["label"].String());
	CHECK(labels.count("./shapes"));
	CHECK(labels.count("./lib/"));
	CHECK(!labels.count("./main")); // the file itself
}

TEST("lsp: the lifecycle's errors; a network share's URI; a script named by its first line; a message without a length")
{
	Project project;
	{
		Session s;
		CHECK_EQ(s.Request("textDocument/hover", At(project.Main, 0, 0))["error"]["code"].Int(), -32002);
		s.Notify("textDocument/didOpen", OpenedDocument(project.Main, "print(missing)\n"));
		CHECK(s.Notes.empty());
		s.Request("initialize", Json::Object());
		s.Request("shutdown", Json::Object());
		CHECK_EQ(s.Request("workspace/symbol", Json::Object().Set("query", "x"))["error"]["code"].Int(), -32600);
		s.Notify("exit", Json::Object());
		CHECK(s.Server.ShouldExit());
		CHECK_EQ(s.Server.ExitCode(), 0);
	}
	CHECK_EQ(LspServer::PathFromUri("file://server/share/dir/x.cluau"), std::string("//server/share/dir/x.cluau"));
	CHECK_EQ(LspServer::UriFromPath("//server/share/dir/x.cluau"), std::string("file://server/share/dir/x.cluau"));
	CHECK_EQ(LspServer::UriFromPath("\\\\server\\share\\x.cluau"), std::string("file://server/share/x.cluau"));
	CHECK_EQ(LspServer::PathFromUri("file://localhost/tmp/x.cluau"), std::string("/tmp/x.cluau"));
	{
		Session s;
		s.Request("initialize", Json::Object());
		const std::string tool = LspServer::UriFromPath((project.Dir / "tool").generic_u8string());
		s.Notify("textDocument/didOpen", OpenedDocument(tool, "#!/usr/bin/env cluau\nprint(missing)\n"));
		CHECK(s.Diagnostics(tool).Size() > 0);
	}

	std::FILE* in = std::tmpfile();
	std::FILE* written = std::tmpfile();
	CHECK(in && written);
	if (!in || !written)
		return;
	const std::string first = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
	const std::string next = R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})";
	const std::string exit = R"({"jsonrpc":"2.0","method":"exit"})";
	std::fprintf(in, "Content-Length: %zu\r\n\r\n%s", first.size(), first.c_str());
	std::fprintf(in, "X-Other: 1\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"shutdown\"}");
	std::fprintf(in, "Content-Length: %zu\r\n\r\n%s", next.size(), next.c_str());
	std::fprintf(in, "Content-Length: %zu\r\n\r\n%s", exit.size(), exit.c_str());
	std::rewind(in);
	LspServer server;
	CHECK_EQ(server.Serve(in, written), 0); // the shutdown after the message without a length was read
	std::rewind(written);
	std::string text;
	for (int c; (c = std::fgetc(written)) != EOF;)
		text += (char)c;
	CHECK(text.find("\"code\":-32700") != std::string::npos);
	CHECK(text.find("\"id\":2") != std::string::npos);
	std::fclose(in);
	std::fclose(written);
}

// ---- The debugger ------------------------------------------------------------

namespace {

	using cluau::runtime::detail::DapServer;

	// Every message is JSON text, so UTF-8: the bytes a strict decoder takes.
	bool IsUtf8(const std::string& text)
	{
		for (size_t i = 0; i < text.size();)
		{
			const unsigned char c = (unsigned char)text[i];
			size_t n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
			if (n == 0 || i + n > text.size())
				return false;
			unsigned code = n == 1 ? c : c & (0x3F >> (n - 1));
			for (size_t k = 1; k < n; k++)
			{
				if (((unsigned char)text[i + k] & 0xC0) != 0x80)
					return false;
				code = (code << 6) | ((unsigned char)text[i + k] & 0x3F);
			}
			if ((n == 3 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) || (n == 4 && (code < 0x10000 || code > 0x10FFFF)))
				return false;
			i += n;
		}
		return true;
	}

	// The debug adapter on a thread of its own, as an editor drives it.
	struct Debug
	{
		std::mutex Mutex;
		std::condition_variable Arrived;
		std::deque<Json> Messages;
		std::vector<std::string> Console; // the debugger's own output
		std::unique_ptr<DapServer> Server;
		std::thread Runner;
		std::filesystem::path Cwd = std::filesystem::current_path();
		int Seq = 0;

		Debug()
		{
			DapServer::Io io;
			io.Send = [this](const std::string& text) {
				if (!IsUtf8(text))
					cluau_test::Fail(__FILE__, __LINE__, "a message that is not UTF-8: " + text);
				std::optional<Json> m = Json::Parse(text);
				std::lock_guard<std::mutex> lock(Mutex);
				if (m && (*m)["event"].String() == "output" && (*m)["body"]["category"].String() == "console")
					Console.push_back((*m)["body"]["output"].String());
				else if (m)
					Messages.push_back(std::move(*m));
				Arrived.notify_all();
			};
			Server = std::make_unique<DapServer>(std::move(io));
			Runner = std::thread([this] { Server->Run(); });
		}

		~Debug()
		{
			Send("disconnect", Json::Object());
			Server->EndOfInput();
			Runner.join();
			std::error_code ec;
			std::filesystem::current_path(Cwd, ec);
		}

		int Send(const std::string& command, Json arguments)
		{
			Server->Receive(Json::Object()
								.Set("seq", ++Seq)
								.Set("type", "request")
								.Set("command", command)
								.Set("arguments", std::move(arguments))
								.Dump());
			return Seq;
		}

		// The first message (in order) that `match` takes; null after 10 seconds.
		template <typename F>
		Json Next(F match, const std::string& what)
		{
			std::unique_lock<std::mutex> lock(Mutex);
			for (;;)
			{
				for (auto it = Messages.begin(); it != Messages.end(); ++it)
					if (match(*it))
					{
						Json m = *it;
						Messages.erase(it);
						return m;
					}
				if (Arrived.wait_for(lock, std::chrono::seconds(10)) == std::cv_status::timeout)
				{
					cluau_test::Fail(__FILE__, __LINE__, "no " + what);
					return {};
				}
			}
		}

		Json Request(const std::string& command, Json arguments = Json::Object())
		{
			const int seq = Send(command, std::move(arguments));
			return Next([&](const Json& m) { return m["type"].String() == "response" && m["request_seq"].Int() == seq; }, command + "'s response");
		}

		Json Event(const std::string& name)
		{
			return Next([&](const Json& m) { return m["type"].String() == "event" && m["event"].String() == name; }, "'" + name + "' event");
		}

		// initialize, launch, the breakpoints, configurationDone.
		void Start(const std::filesystem::path& program, const std::vector<std::pair<std::filesystem::path, Json>>& breakpoints, Json launch = Json::Object(),
			Json filters = Json::Array().Push("uncaught"))
		{
			Request("initialize", Json::Object().Set("adapterID", "cluau"));
			launch.Set("program", program.u8string()).Set("cwd", program.parent_path().u8string());
			CHECK(Request("launch", std::move(launch))["success"].Bool());
			for (const auto& [file, list] : breakpoints)
				Request("setBreakpoints", Json::Object().Set("source", Json::Object().Set("path", file.u8string())).Set("breakpoints", list));
			Request("setExceptionBreakpoints", Json::Object().Set("filters", std::move(filters)));
			Request("configurationDone");
		}

		// The next pause: its reason, and the stack's top frame.
		Json Stopped(std::string* reason = nullptr)
		{
			const Json stopped = Event("stopped");
			if (reason)
				*reason = stopped["body"]["reason"].String();
			return Request("stackTrace", Json::Object().Set("threadId", 1))["body"]["stackFrames"];
		}

		// A frame's locals (or a table's fields), by name.
		std::map<std::string, Json> Variables(int reference)
		{
			std::map<std::string, Json> out;
			const Json response = Request("variables", Json::Object().Set("variablesReference", reference));
			for (const Json& v : response["body"]["variables"].Items())
				out[v["name"].String()] = v;
			return out;
		}

		std::map<std::string, Json> Locals(int frame = 1)
		{
			const Json scopes = Request("scopes", Json::Object().Set("frameId", frame))["body"]["scopes"];
			return Variables(scopes[0]["variablesReference"].Int());
		}
	};

	Json Line(int line) { return Json::Object().Set("line", line); }

	// A script and a module in a directory of their own.
	struct Scripts
	{
		std::filesystem::path Dir, Main, Geo;

		explicit Scripts(const std::string& main)
		{
			std::filesystem::create_directories(TempDir() / "dap");
			Dir = std::filesystem::canonical(TempDir() / "dap");
			Main = Dir / "main.cluau";
			Geo = Dir / "geo.cluau";
			std::ofstream(Geo, std::ios::binary) << "class Point {\n"           // 1
													"\tx: number = 0\n"          // 2
													"\ty: number = 0\n"          // 3
													"\tfunction Init(x: number, y: number) {\n"
													"\t\tself.x = x\n"           // 5
													"\t\tself.y = y\n"           // 6
													"\t}\n"                      // 7
													"\tfunction Length(): number {\n"
													"\t\tlet squared = self.x * self.x + self.y * self.y\n" // 9
													"\t\treturn math.sqrt(squared)\n" // 10
													"\t}\n"
													"}\n"
													"return { Point = Point }\n";
			std::ofstream(Main, std::ios::binary) << main;
		}
	};

} // namespace

TEST("debug: a breakpoint pauses with the frame's locals, its stack and expressions in it")
{
	Scripts scripts("const geo = require(\"./geo\")\n" // 1
					"let p = geo.Point(3, 4)\n"          // 2
					"let len = p:Length()\n"             // 3
					"print(len)\n");                     // 4
	Debug d;
	d.Start(scripts.Main, { { scripts.Geo, Json::Array().Push(Line(10)) } });
	const Json frames = d.Stopped();
	CHECK_EQ(frames.Size(), size_t(2));
	CHECK_EQ(frames[0]["name"].String(), std::string("Length"));
	CHECK_EQ(frames[0]["line"].Int(), 10);
	CHECK_EQ(frames[0]["source"]["path"].String(), scripts.Geo.u8string());
	CHECK_EQ(frames[1]["name"].String(), std::string("(top level)"));
	CHECK_EQ(frames[1]["line"].Int(), 3);

	std::map<std::string, Json> locals = d.Locals();
	CHECK_EQ(locals["squared"]["value"].String(), std::string("25"));
	CHECK_EQ(locals["self"]["type"].String(), std::string("Point"));
	std::map<std::string, Json> fields = d.Variables(locals["self"]["variablesReference"].Int());
	CHECK_EQ(fields["x"]["value"].String(), std::string("3"));
	CHECK_EQ(fields["y"]["value"].String(), std::string("4"));
	const std::string p = d.Locals(2)["p"]["value"].String(); // the fields in the table's order
	CHECK(p.rfind("Point {", 0) == 0 && p.find("x = 3") != std::string::npos && p.find("y = 4") != std::string::npos);

	// cluau's operators, in the frame's scope.
	const Json value = d.Request("evaluate", Json::Object().Set("expression", "squared != 25 || self.x + self.y == 7").Set("frameId", 1));
	CHECK_EQ(value["body"]["result"].String(), std::string("true"));
	const Json broken = d.Request("evaluate", Json::Object().Set("expression", "squared +").Set("frameId", 1));
	CHECK(!broken["success"].Bool());

	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: stepping over, into and out")
{
	Scripts scripts("const geo = require(\"./geo\")\n" // 1
					"let p = geo.Point(3, 4)\n"          // 2
					"let len = p:Length()\n"             // 3
					"let twice = len * 2\n");            // 4
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(2)) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 2);
	d.Request("next", Json::Object().Set("threadId", 1));
	std::string reason;
	CHECK_EQ(d.Stopped(&reason)[0]["line"].Int(), 3);
	CHECK_EQ(reason, std::string("step"));
	d.Request("stepIn", Json::Object().Set("threadId", 1));
	Json frames = d.Stopped();
	CHECK_EQ(frames[0]["name"].String(), std::string("Length"));
	CHECK_EQ(frames[0]["line"].Int(), 9);
	d.Request("next", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 10);
	d.Request("stepOut", Json::Object().Set("threadId", 1));
	frames = d.Stopped();
	CHECK_EQ(frames[0]["source"]["path"].String(), scripts.Main.u8string());
	CHECK(frames[0]["line"].Int() >= 3);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: a step enters a module's top level, and steps through a coroutine")
{
	Scripts scripts("const geo = require(\"./geo\")\n"                 // 1
					"let co = coroutine.create(function(n: number) {\n" // 2
					"\tlet doubled = n * 2\n"                           // 3
					"\tcoroutine.yield(doubled)\n"                      // 4
					"\treturn doubled + 1\n"                            // 5
					"})\n"                                              // 6
					"let _, first = coroutine.resume(co, 5)\n"          // 7
					"let _, second = coroutine.resume(co)\n"            // 8
					"let sum = first + second\n");                      // 9
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(1)).Push(Line(3)) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 1);
	d.Request("stepIn", Json::Object().Set("threadId", 1));
	Json frames = d.Stopped();
	CHECK_EQ(frames[0]["source"]["path"].String(), scripts.Geo.u8string());
	CHECK_EQ(frames[0]["name"].String(), std::string("(top level)"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 3);
	d.Request("next", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 4);
	CHECK_EQ(d.Locals()["doubled"]["value"].String(), std::string("10"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: a module that fails can be required again, and a cycle is still one")
{
	Scripts scripts("let first = \"\"\n"
					"try {\n"
					"\trequire(\"./bad\")\n"
					"} catch (error) {\n"
					"\tfirst = tostring(error)\n"
					"}\n"
					"let second = \"\"\n"
					"try {\n"
					"\trequire(\"./bad\")\n"
					"} catch (error) {\n"
					"\tsecond = tostring(error)\n"
					"}\n"
					"let cycle = \"\"\n"
					"try {\n"
					"\trequire(\"./loop\")\n"
					"} catch (error) {\n"
					"\tcycle = tostring(error)\n"
					"}\n"
					"print(first)\n"); // 19
	std::ofstream(scripts.Dir / "bad.cluau", std::ios::binary) << "error(\"bad module\")\n";
	std::ofstream(scripts.Dir / "loop.cluau", std::ios::binary) << "require(\"./loop\")\n";
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(19)) } });
	d.Stopped();
	std::map<std::string, Json> locals = d.Locals();
	CHECK(locals["first"]["value"].String().find("bad module") != std::string::npos);
	CHECK(locals["second"]["value"].String().find("bad module") != std::string::npos);
	CHECK(locals["cycle"]["value"].String().find("require cycle") != std::string::npos);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: a step inside a function called from C (a sort comparator)")
{
	Scripts scripts("let xs = { 3, 1, 2 }\n"               // 1
					"table.sort(xs, function(a, b) {\n"     // 2
					"\tlet less = a < b\n"                  // 3
					"\treturn less\n"                       // 4
					"})\n"                                  // 5
					"let first = xs[1]\n");                 // 6
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(3)) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 3);
	d.Request("setBreakpoints", Json::Object().Set("source", Json::Object().Set("path", scripts.Main.u8string())).Set("breakpoints", Json::Array()));
	d.Request("next", Json::Object().Set("threadId", 1));
	std::string reason;
	CHECK_EQ(d.Stopped(&reason)[0]["line"].Int(), 4);
	CHECK_EQ(reason, std::string("step"));
	CHECK(d.Locals().count("less"));
	d.Request("stepOut", Json::Object().Set("threadId", 1));
	const Json frames = d.Stopped();
	CHECK_EQ(frames[0]["line"].Int(), 6);
	CHECK_EQ(frames[0]["name"].String(), std::string("(top level)"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: a test file's cases run under the debugger, filtered as cluau test -k filters them")
{
	Scripts scripts("");
	const std::filesystem::path file = scripts.Dir / "math_test.cluau";
	std::ofstream(file, std::ios::binary) << "const test = require(\"@std/test\")\n" // 1
											 "test.case(\"adds\", function() {\n"    // 2
											 "\tlet sum = 1 + 2\n"                    // 3
											 "\ttest.equal(sum, 3)\n"                 // 4
											 "})\n"                                   // 5
											 "test.case(\"fails\", function() {\n"   // 6
											 "\ttest.equal(1, 2)\n"                   // 7
											 "})\n";                                  // 8
	{
		Debug d;
		d.Start(file, { { file, Json::Array().Push(Line(4)) } }, Json::Object().Set("test", true));
		Json frames = d.Stopped();
		CHECK_EQ(frames[0]["line"].Int(), 4);
		CHECK_EQ(d.Locals()["sum"]["value"].String(), std::string("3"));
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 1);
		std::lock_guard<std::mutex> lock(d.Mutex);
		CHECK(!d.Console.empty() && d.Console.back() == "1 passed, 1 failed\n");
	}
	{
		Debug d;
		d.Start(file, {}, Json::Object().Set("test", true).Set("filter", "add"));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	}
}

TEST("debug: conditions, hit counts and log messages")
{
	Scripts scripts("let total = 0\n"        // 1
					"for (i = 1, 10) {\n"     // 2
					"\ttotal += i\n"          // 3
					"}\n");                   // 4
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array()
												 .Push(Json::Object().Set("line", 3).Set("logMessage", "i={i}"))
												 .Push(Json::Object().Set("line", 3).Set("condition", "i > 4 && i != 6").Set("hitCondition", "2")) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 3);
	CHECK_EQ(d.Locals()["i"]["value"].String(), std::string("7")); // the second hit with i > 4 and not 6
	d.Request("setBreakpoints", Json::Object()
									.Set("source", Json::Object().Set("path", scripts.Main.u8string()))
									.Set("breakpoints", Json::Array().Push(Json::Object().Set("line", 3).Set("condition", "nope +"))));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	std::lock_guard<std::mutex> lock(d.Mutex);
	CHECK(d.Console.size() >= 7);
	if (d.Console.size() >= 7)
	{
		CHECK_EQ(d.Console[0], std::string("i=1\n"));
		CHECK_EQ(d.Console[6], std::string("i=7\n"));
	}
}

TEST("debug: a breakpoint that does not compile is not verified; one on a blank line moves to the code")
{
	Scripts scripts("let a = 1\n" // 1
					"\n"          // 2
					"let b = 2\n" // 3
					"print(a + b)\n");
	Debug d;
	d.Request("initialize", Json::Object());
	const Json set = d.Request("setBreakpoints", Json::Object()
												  .Set("source", Json::Object().Set("path", scripts.Main.u8string()))
												  .Set("breakpoints", Json::Array().Push(Line(2))));
	CHECK_EQ(set["body"]["breakpoints"][0]["line"].Int(), 2);
	d.Request("launch", Json::Object().Set("program", scripts.Main.u8string()));
	d.Request("configurationDone");
	const Json moved = d.Event("breakpoint");
	CHECK_EQ(moved["body"]["breakpoint"]["line"].Int(), 3);
	CHECK_EQ(moved["body"]["breakpoint"]["id"].Int(), set["body"]["breakpoints"][0]["id"].Int());
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 3);

	const Json bad = d.Request("setBreakpoints", Json::Object()
												  .Set("source", Json::Object().Set("path", scripts.Main.u8string()))
												  .Set("breakpoints", Json::Array().Push(Json::Object().Set("line", 4).Set("condition", "a +"))));
	CHECK(!bad["body"]["breakpoints"][0]["verified"].Bool());
	CHECK(bad["body"]["breakpoints"][0]["message"].String().find("condition") != std::string::npos);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: errors pause where they are raised, caught ones when asked")
{
	Scripts scripts("function check(n: number) {\n" // 1
					"\tif (n > 1) {\n"             // 2
					"\t\tthrow `too big: {n}`\n"   // 3
					"\t}\n"                        // 4
					"}\n"                           // 5
					"try {\n"                       // 6
					"\tcheck(2)\n"                 // 7
					"} catch (error) {\n"           // 8
					"}\n"                           // 9
					"check(3)\n");                  // 10
	{
		Debug d;
		d.Start(scripts.Main, {}, Json::Object(), Json::Array().Push("all"));
		std::string reason;
		Json frames = d.Stopped(&reason);
		CHECK_EQ(reason, std::string("exception"));
		CHECK_EQ(frames[0]["line"].Int(), 3);
		CHECK_EQ(d.Locals()["n"]["value"].String(), std::string("2"));
		CHECK_EQ(d.Request("exceptionInfo", Json::Object().Set("threadId", 1))["body"]["breakMode"].String(), std::string("always"));
		d.Request("continue", Json::Object().Set("threadId", 1));
		frames = d.Stopped(&reason);
		CHECK_EQ(d.Locals()["n"]["value"].String(), std::string("3"));
		CHECK_EQ(d.Request("exceptionInfo", Json::Object().Set("threadId", 1))["body"]["breakMode"].String(), std::string("unhandled"));
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 1);
	}
	{
		Debug d;
		d.Start(scripts.Main, {});
		d.Stopped();
		CHECK_EQ(d.Locals()["n"]["value"].String(), std::string("3")); // the caught one passed
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 1);
	}
}

TEST("debug: stopping on entry, pausing a running script, and ending it")
{
	Scripts scripts("let spins = 0\n"     // 1
					"while (true) {\n"    // 2
					"\tspins += 1\n"      // 3
					"}\n");               // 4
	Debug d;
	d.Start(scripts.Main, {}, Json::Object().Set("stopOnEntry", true));
	std::string reason;
	CHECK_EQ(d.Stopped(&reason)[0]["line"].Int(), 1);
	CHECK_EQ(reason, std::string("entry"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	d.Request("pause", Json::Object().Set("threadId", 1));
	d.Stopped(&reason);
	CHECK_EQ(reason, std::string("pause"));
	CHECK(d.Locals()["spins"]["value"].String() != "0");
	d.Request("terminate");
	d.Event("terminated");
}

TEST("debug: a step follows a coroutine in, and out where it yields or returns")
{
	Scripts scripts("let co = coroutine.create(function(n: number) {\n" // 1
					"\tlet x = n\n"                                     // 2
					"\tcoroutine.yield(x)\n"                            // 3
					"\tx = x + 1\n"                                     // 4
					"\treturn x\n"                                      // 5
					"})\n"                                              // 6
					"let _, first = coroutine.resume(co, 1)\n"          // 7
					"let _, second = coroutine.resume(co)\n"            // 8
					"let wrapped = coroutine.wrap(function() {\n"       // 9
					"\tlet y = 1\n"                                     // 10
					"\tcoroutine.yield(y)\n"                            // 11
					"})\n"                                              // 12
					"wrapped()\n"                                       // 13
					"let sum = first + second\n");                      // 14
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(7)) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 7);
	d.Request("setBreakpoints", Json::Object().Set("source", Json::Object().Set("path", scripts.Main.u8string())).Set("breakpoints", Json::Array()));
	d.Request("stepIn", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 2); // in the coroutine
	d.Request("next", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 3);
	d.Request("next", Json::Object().Set("threadId", 1)); // over the yield: back where it was resumed
	Json frames = d.Stopped();
	CHECK_EQ(frames[0]["line"].Int(), 8);
	CHECK_EQ(frames[0]["name"].String(), std::string("(top level)"));
	d.Request("stepIn", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 4);
	d.Request("stepOut", Json::Object().Set("threadId", 1)); // out at its return
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 9);
	d.Request("next", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 13);
	d.Request("stepIn", Json::Object().Set("threadId", 1)); // a function coroutine.wrap made
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 10);
	d.Request("stepOut", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 14);
	d.Request("next", Json::Object().Set("threadId", 1)); // not onto the file's end
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: a step into a callback C calls again stops at each call")
{
	Scripts scripts("let out = string.gsub(\"abc\", \"%w\", function(c: string): string {\n" // 1
					"\treturn c:upper()\n"                                                  // 2
					"})\n"                                                                  // 3
					"print(out)\n");                                                        // 4
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(1)) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 1);
	d.Request("setBreakpoints", Json::Object().Set("source", Json::Object().Set("path", scripts.Main.u8string())).Set("breakpoints", Json::Array()));
	std::vector<std::string> letters;
	for (int i = 0; i < 3; i++)
	{
		d.Request("stepIn", Json::Object().Set("threadId", 1));
		const Json frames = d.Stopped();
		CHECK_EQ(frames[0]["line"].Int(), 2);
		letters.push_back(d.Locals()["c"]["value"].String());
	}
	CHECK(letters == std::vector<std::string>({ "\"a\"", "\"b\"", "\"c\"" }));
	d.Request("stepIn", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 4);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: requests are served while the script waits: breakpoints, a pause, its end")
{
	Scripts scripts("const task = require(\"@std/task\")\n" // 1
					"task.wait(1.5)\n"                       // 2
					"let after = 1\n"                        // 3
					"print(after)\n");                       // 4
	{
		Debug d;
		d.Start(scripts.Main, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto start = std::chrono::steady_clock::now();
		const Json set = d.Request("setBreakpoints", Json::Object()
														 .Set("source", Json::Object().Set("path", scripts.Main.u8string()))
														 .Set("breakpoints", Json::Array().Push(Line(4))));
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(800)); // during the wait
		CHECK(set["body"]["breakpoints"][0]["verified"].Bool());
		CHECK_EQ(d.Stopped()[0]["line"].Int(), 4);
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	}
	{
		Debug d;
		d.Start(scripts.Main, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		d.Request("pause", Json::Object().Set("threadId", 1));
		std::string reason;
		CHECK_EQ(d.Stopped(&reason)[0]["source"]["path"].String(), scripts.Main.u8string()); // the script's next line
		CHECK_EQ(reason, std::string("pause"));
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	}
	{
		Debug d;
		d.Start(scripts.Main, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto start = std::chrono::steady_clock::now();
		CHECK(d.Request("terminate")["success"].Bool());
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0); // ended as asked, not failed
		d.Event("terminated");
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(800));
	}
}

TEST("debug: ending a paused script; breakpoints past the file's end; a test file by its name")
{
	Scripts scripts("let a = 1\n"  // 1
					"print(a)\n"); // 2
	{
		Debug d;
		d.Request("initialize", Json::Object());
		const Json set = d.Request("setBreakpoints", Json::Object()
														 .Set("source", Json::Object().Set("path", scripts.Main.u8string()))
														 .Set("breakpoints", Json::Array().Push(Line(2)).Push(Line(3)).Push(Line(40))));
		CHECK(!set["body"]["breakpoints"][0]["verified"].Bool()); // until the script loads the file
		CHECK_EQ(set["body"]["breakpoints"][0]["reason"].String(), std::string("pending"));
		CHECK(!set["body"]["breakpoints"][1]["verified"].Bool()); // the line after the last line break
		CHECK_EQ(set["body"]["breakpoints"][1]["reason"].String(), std::string("failed"));
		CHECK(!set["body"]["breakpoints"][2]["verified"].Bool());
		d.Request("launch", Json::Object().Set("program", scripts.Main.u8string()));
		d.Request("configurationDone");
		const int id = set["body"]["breakpoints"][0]["id"].Int();
		const Json verified = d.Next([&](const Json& m) { return m["event"].String() == "breakpoint" && m["body"]["breakpoint"]["id"].Int() == id; }, "breakpoint event");
		CHECK(verified["body"]["breakpoint"]["verified"].Bool());
		CHECK_EQ(d.Stopped()[0]["line"].Int(), 2);
		d.Request("terminate");
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
		d.Event("terminated");
	}
	const std::filesystem::path file = scripts.Dir / "sum_test.cluau";
	std::ofstream(file, std::ios::binary) << "const test = require(\"@std/test\")\n"
											 "test.case(\"adds\", function() {\n"
											 "\ttest.equal(1 + 2, 3)\n"
											 "})\n";
	{
		Debug d;
		d.Start(file, {}); // no "test": a *_test.cluau file's cases run
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
		std::lock_guard<std::mutex> lock(d.Mutex);
		CHECK(!d.Console.empty() && d.Console.back() == "1 passed\n");
	}
}

TEST("debug: All Errors pauses at an error coroutine.resume catches")
{
	Scripts scripts("let co = coroutine.create(function(n: number) {\n" // 1
					"\terror(`bad {n}`)\n"                              // 2
					"})\n"                                              // 3
					"let ok, message = coroutine.resume(co, 4)\n"       // 4
					"print(ok, message)\n");                            // 5
	Debug d;
	d.Start(scripts.Main, {}, Json::Object(), Json::Array().Push("all"));
	std::string reason;
	const Json frames = d.Stopped(&reason);
	CHECK_EQ(reason, std::string("exception"));
	CHECK_EQ(frames[0]["line"].Int(), 2);
	CHECK_EQ(d.Locals()["n"]["value"].String(), std::string("4"));
	CHECK_EQ(d.Request("exceptionInfo", Json::Object().Set("threadId", 1))["body"]["breakMode"].String(), std::string("always"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: a table holding a long list opens, as the list does; a value that does not return is stopped")
{
	Scripts scripts("const data = { name = \"rows\", rows = { 1, 2, 3, 4, 5, 6, 7, 8 } }\n" // 1
					"const seven = { 1, 2, 3, 4, 5, 6, 7 }\n"                               // 2
					"class Slow {\n"                                                        // 3
					"\tfunction __tostring(): string {\n"                                   // 4
					"\t\twhile (true) { }\n"                                                // 5
					"\t\treturn \"never\"\n"                                                // 6
					"\t}\n"                                                                 // 7
					"}\n"                                                                   // 8
					"const slow = Slow()\n"                                                 // 9
					"print(data.name)\n");                                                  // 10
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(10)) } });
	d.Stopped();
	std::map<std::string, Json> locals = d.Locals();
	CHECK_EQ(locals["seven"]["type"].String(), std::string("table"));
	CHECK_EQ(locals["seven"]["value"].String(), std::string("{1, 2, 3, 4, 5, 6, ...}"));
	CHECK_EQ(d.Variables(locals["seven"]["variablesReference"].Int()).size(), size_t(7));
	std::map<std::string, Json> fields = d.Variables(locals["data"]["variablesReference"].Int());
	CHECK_EQ(fields["name"]["value"].String(), std::string("\"rows\""));
	CHECK_EQ(fields["rows"]["value"].String(), std::string("{1, 2, 3, 4, 5, 6, ...}"));
	CHECK_EQ(d.Variables(fields["rows"]["variablesReference"].Int()).size(), size_t(8));
	CHECK(locals["slow"]["value"].String().find("__tostring failed") != std::string::npos); // stopped, not hung

	const auto start = std::chrono::steady_clock::now();
	const Json forever = d.Request("evaluate", Json::Object().Set("expression", "(function() { while (true) { } })()").Set("frameId", 1));
	CHECK(!forever["success"].Bool());
	CHECK(forever["message"].String().find("more than two seconds") != std::string::npos);
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
	CHECK_EQ(d.Request("evaluate", Json::Object().Set("expression", "#seven").Set("frameId", 1))["body"]["result"].String(), std::string("7"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}

TEST("debug: strings are cut at a character's end; bytes, keys and vectors are shown as they are")
{
	Scripts scripts("const quotes = string.rep('\"', 60)\n"                        // 1
					"const accents = \"ab\" .. string.rep(\"\xC3\xA9\", 50)\n"     // 2
					"const raw = \"bad \\xff byte\\0\\r\"\n"                        // 3
					"const keys: any = { [1] = \"one\", [\"[1]\"] = \"two\", [\"a b\"] = 3 }\n" // 4
					"const v = vector.create(1, 2, 3)\n"                           // 5
					"const many: { number } = {}\n"                                // 6
					"for (i = 1, 600) { many[i] = i }\n"                           // 7
					"print(\"a bad byte: \\xfe\")\n");                              // 8
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(8)) } });
	d.Stopped();
	std::map<std::string, Json> locals = d.Locals();
	const std::string quotes = locals["quotes"]["value"].String();
	CHECK(quotes.size() > 8 && quotes.compare(quotes.size() - 4, 4, "...\"") == 0);
	const std::string accents = locals["accents"]["value"].String();
	CHECK(accents.size() > 8 && accents.compare(accents.size() - 6, 6, "\xC3\xA9...\"") == 0);
	CHECK_EQ(locals["raw"]["value"].String(), std::string("\"bad \\xFF byte\\0\\r\""));
	CHECK_EQ(locals["v"]["value"].String(), std::string("vector(1, 2, 3)"));
	std::map<std::string, Json> keys = d.Variables(locals["keys"]["variablesReference"].Int());
	CHECK_EQ(keys["[1]"]["value"].String(), std::string("\"one\""));
	CHECK_EQ(keys["[\"[1]\"]"]["value"].String(), std::string("\"two\""));
	CHECK_EQ(keys["[\"a b\"]"]["value"].String(), std::string("3"));
	const Json many = d.Request("variables", Json::Object().Set("variablesReference", locals["many"]["variablesReference"].Int()));
	const Json& children = many["body"]["variables"];
	CHECK_EQ(children.Size(), size_t(501));
	CHECK_EQ(children[500]["name"].String(), std::string("..."));
	CHECK(children[500]["value"].String().find("100 more of 600") != std::string::npos);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0); // its output, a bad byte in it, was UTF-8 too
}

TEST("debug: expressions see a local holding nil over a global, and the frame's varargs")
{
	Scripts scripts("count = 5\n"                  // 1
					"function F(first: string, ...: any) {\n" // 2
					"\tlet count = nil\n"          // 3
					"\tprint(count, first)\n"      // 4
					"}\n"                          // 5
					"F(\"x\", \"y\", \"z\")\n");   // 6
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(4)).Push(Json::Object().Set("line", 4).Set("condition", "count == nil").Set("logMessage", "count is {count}")) } });
	d.Stopped();
	auto eval = [&](const std::string& expression) {
		return d.Request("evaluate", Json::Object().Set("expression", expression).Set("frameId", 1))["body"]["result"].String();
	};
	CHECK_EQ(eval("count"), std::string("nil"));
	CHECK_EQ(eval("first"), std::string("\"x\""));
	CHECK_EQ(eval("select('#', ...)"), std::string("2"));
	CHECK_EQ(eval("(...)"), std::string("\"y\""));
	for (int frame : { 0, -1, 99 })
		CHECK(!d.Request("evaluate", Json::Object().Set("expression", "1").Set("frameId", frame))["success"].Bool());
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	std::lock_guard<std::mutex> lock(d.Mutex);
	CHECK(!d.Console.empty() && d.Console.back() == "count is nil\n"); // the condition held, and the log ran
}

TEST("debug: every breakpoint at a line counts; a hit count past int's range is refused")
{
	Scripts scripts("let total = 0\n"        // 1
					"for (i = 1, 3) {\n"     // 2
					"\n"                     // 3
					"\ttotal += i\n"         // 4
					"}\n"                    // 5
					"print(total)\n");       // 6
	Debug d;
	d.Request("initialize", Json::Object());
	const Json set = d.Request("setBreakpoints",
		Json::Object()
			.Set("source", Json::Object().Set("path", scripts.Main.u8string()))
			.Set("breakpoints", Json::Array()
									.Push(Line(3))
									.Push(Json::Object().Set("line", 4).Set("logMessage", "total before: {total}"))
									.Push(Json::Object().Set("line", 4).Set("hitCondition", "2"))
									.Push(Json::Object().Set("line", 6).Set("hitCondition", "4294967296"))));
	CHECK(!set["body"]["breakpoints"][3]["verified"].Bool());
	CHECK(set["body"]["breakpoints"][3]["message"].String().find("hit count") != std::string::npos);
	const int first = set["body"]["breakpoints"][0]["id"].Int(), third = set["body"]["breakpoints"][2]["id"].Int();
	d.Request("launch", Json::Object().Set("program", scripts.Main.u8string()));
	d.Request("configurationDone");
	std::vector<int> ids;
	for (int pass = 1; pass <= 3; pass++)
	{
		const Json stopped = d.Event("stopped");
		ids.clear();
		for (const Json& id : stopped["body"]["hitBreakpointIds"].Items())
			ids.push_back(id.Int());
		CHECK_EQ(ids.size(), size_t(pass == 1 ? 1 : 2));
		CHECK_EQ(ids[0], first);
		if (pass > 1)
			CHECK_EQ(ids[1], third);
		d.Request("continue", Json::Object().Set("threadId", 1));
	}
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	std::lock_guard<std::mutex> lock(d.Mutex);
	CHECK_EQ(d.Console.size(), size_t(3));
	CHECK(d.Console.size() == 3 && d.Console[2] == "total before: 3\n");
}

TEST("debug: a step over task.wait in a task stops at its next line; a task's error pauses in the task")
{
	Scripts scripts("const task = require(\"@std/task\")\n"  // 1
					"const worker = task.spawn(function() {\n" // 2
					"\tfor (i = 1, 2) {\n"                     // 3
					"\t\tlet w = i\n"                          // 4
					"\t\ttask.wait(0.05)\n"                    // 5
					"\t}\n"                                    // 6
					"\tlet local1 = \"in the task\"\n"         // 7
					"\terror(\"task failure\")\n"              // 8
					"})\n"                                     // 9
					"task.wait(0.5)\n");                       // 10
	Debug d;
	d.Start(scripts.Main, { { scripts.Main, Json::Array().Push(Line(5)) } });
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 5);
	d.Request("setBreakpoints", Json::Object().Set("source", Json::Object().Set("path", scripts.Main.u8string())).Set("breakpoints", Json::Array()));
	d.Request("next", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 3); // after the wait, in the task
	d.Request("next", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 4);
	CHECK_EQ(d.Locals()["i"]["value"].String(), std::string("2"));
	d.Request("continue", Json::Object().Set("threadId", 1));
	std::string reason;
	const Json frames = d.Stopped(&reason);
	CHECK_EQ(reason, std::string("exception"));
	CHECK_EQ(frames[0]["line"].Int(), 8);
	CHECK_EQ(frames[0]["source"]["path"].String(), scripts.Main.u8string());
	CHECK_EQ(d.Locals()["local1"]["value"].String(), std::string("\"in the task\""));
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 1); // and no second pause for it
}

TEST("debug: All Errors leaves out the errors a standard module raises and catches itself")
{
	Scripts scripts("const server = require(\"@std/server\")\n"                    // 1
					"const net = require(\"@std/net\")\n"                          // 2
					"const task = require(\"@std/task\")\n"                        // 3
					"const s = server.serve({ host = \"127.0.0.1\", port = 0, idle = 0.2 }, function(request: server.Request): server.Answer {\n"
					"\treturn \"hi\"\n"                                            // 5
					"})\n"                                                         // 6
					"const c = net.connect(\"127.0.0.1\", s.port)\n"              // 7
					"task.wait(0.6)\n"                                             // 8
					"c:close()\n"                                                  // 9
					"s:close()\n"                                                  // 10
					"try {\n"                                                      // 11
					"\terror(\"mine\")\n"                                          // 12
					"} catch (e) {\n"                                              // 13
					"}\n");                                                        // 14
	{
		Debug d;
		d.Start(scripts.Main, {}, Json::Object(), Json::Array().Push("all"));
		std::string reason;
		const Json frames = d.Stopped(&reason);
		CHECK_EQ(reason, std::string("exception"));
		CHECK_EQ(frames[0]["line"].Int(), 12);
		CHECK_EQ(frames[0]["source"]["path"].String(), scripts.Main.u8string());
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	}
	// A failing assertion: @std/test raises it and catches it, through the case's code.
	const std::filesystem::path file = scripts.Dir / "fails_test.cluau";
	std::ofstream(file, std::ios::binary) << "const test = require(\"@std/test\")\n"
											 "test.case(\"fails\", function() {\n"
											 "\ttest.equal(1, 2)\n"
											 "})\n";
	{
		Debug d;
		d.Start(file, {}, Json::Object(), Json::Array().Push("all"));
		std::string reason;
		const Json frames = d.Stopped(&reason);
		CHECK_EQ(reason, std::string("exception"));
		bool inCase = false;
		for (const Json& frame : frames.Items())
			inCase = inCase || (frame["line"].Int() == 3 && frame["source"]["path"].String() == file.u8string());
		CHECK(inCase);
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 1);
	}
}

TEST("debug: process.exit ends the script, and the session goes on to the editor's disconnect")
{
	Scripts scripts("const process = require(\"@std/process\")\n"
					"print(\"bye\")\n"
					"try {\n"
					"\tprocess.exit(3)\n"
					"} catch (e) {\n"
					"}\n"
					"while (true) { }\n");
	Debug d;
	d.Start(scripts.Main, {});
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 3);
	d.Event("terminated");
	CHECK(d.Request("disconnect")["success"].Bool());
}

TEST("debug: requests are served during a blocking call without @std/task")
{
	Scripts scripts("const process = require(\"@std/process\")\n" // 1
					"process.sleep(1.5)\n"                         // 2
					"let after = 1\n"                              // 3
					"print(after)\n");                             // 4
	{
		Debug d;
		d.Start(scripts.Main, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto start = std::chrono::steady_clock::now();
		const Json set = d.Request("setBreakpoints", Json::Object()
														 .Set("source", Json::Object().Set("path", scripts.Main.u8string()))
														 .Set("breakpoints", Json::Array().Push(Line(4))));
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(800)); // during the sleep
		CHECK(set["body"]["breakpoints"][0]["verified"].Bool());
		CHECK(d.Request("threads")["success"].Bool());
		CHECK_EQ(d.Stopped()[0]["line"].Int(), 4);
		d.Request("continue", Json::Object().Set("threadId", 1));
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
	}
	{
		Debug d;
		d.Start(scripts.Main, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto start = std::chrono::steady_clock::now();
		d.Request("pause", Json::Object().Set("threadId", 1));
		std::string reason;
		CHECK_EQ(d.Stopped(&reason)[0]["source"]["path"].String(), scripts.Main.u8string()); // as the sleep returns
		CHECK_EQ(reason, std::string("pause"));
		d.Request("terminate");
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
		d.Event("terminated");
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2500));
	}
	{
		Debug d;
		d.Start(scripts.Main, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto start = std::chrono::steady_clock::now();
		CHECK(d.Request("terminate")["success"].Bool());
		CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
		d.Event("terminated");
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(800));
	}
}

TEST("debug: a breakpoint in a file not loaded yet is verified as it loads; one on a closing brace")
{
	Scripts scripts("let total = 0\n"            // 1
					"for (i = 1, 3) {\n"         // 2
					"\ttotal += i\n"             // 3
					"}\n"                        // 4
					"function Half(n: number): number {\n" // 5
					"\treturn n / 2\n"           // 6
					"}\n"                        // 7
					"const later = require(\"./later\")\n" // 8
					"print(Half(total), later)\n"); // 9
	std::ofstream(scripts.Dir / "later.cluau", std::ios::binary) << "let x = 1\n"
																	"return x + 1\n";
	Debug d;
	d.Request("initialize", Json::Object());
	const Json set = d.Request("setBreakpoints", Json::Object()
													  .Set("source", Json::Object().Set("path", (scripts.Dir / "later.cluau").u8string()))
													  .Set("breakpoints", Json::Array().Push(Line(2))));
	CHECK(!set["body"]["breakpoints"][0]["verified"].Bool());
	const int later = set["body"]["breakpoints"][0]["id"].Int();
	const Json main = d.Request("setBreakpoints", Json::Object()
													   .Set("source", Json::Object().Set("path", scripts.Main.u8string()))
													   .Set("breakpoints", Json::Array().Push(Line(4)).Push(Line(7))));
	d.Request("launch", Json::Object().Set("program", scripts.Main.u8string()));
	d.Request("configurationDone");
	// The main file loads: its breakpoints go to the line with code before each `}`.
	std::map<int, int> lines;
	for (int n = 0; n < 2; n++)
	{
		const Json b = d.Event("breakpoint")["body"]["breakpoint"];
		CHECK(b["verified"].Bool());
		lines[b["id"].Int()] = b["line"].Int();
	}
	CHECK_EQ(lines[main["body"]["breakpoints"][0]["id"].Int()], 3);
	CHECK_EQ(lines[main["body"]["breakpoints"][1]["id"].Int()], 6);
	for (int pass = 1; pass <= 3; pass++)
	{
		CHECK_EQ(d.Stopped()[0]["line"].Int(), 3); // on each pass of the loop
		d.Request("continue", Json::Object().Set("threadId", 1));
	}
	const Json loaded = d.Event("breakpoint")["body"]["breakpoint"];
	CHECK(loaded["verified"].Bool());
	CHECK_EQ(loaded["id"].Int(), later);
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 2);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Stopped()[0]["line"].Int(), 6);
	d.Request("continue", Json::Object().Set("threadId", 1));
	CHECK_EQ(d.Event("exited")["body"]["exitCode"].Int(), 0);
}
