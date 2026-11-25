#include "test.h"

#include "cluau/runtime.h"
#include "DapServer.h"
#include "Json.h"
#include "LspServer.h"

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

// ---- The debugger ------------------------------------------------------------

namespace {

	using cluau::runtime::detail::DapServer;

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
