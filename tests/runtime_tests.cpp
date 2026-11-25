#include "test.h"

#include "cluau/runtime.h"
#include "Json.h"
#include "LspServer.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

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
			Dir = TempDir() / "lsp";
			std::filesystem::create_directories(Dir);
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
