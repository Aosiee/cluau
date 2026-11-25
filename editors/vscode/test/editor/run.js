// Runs the extension in an editor against a built cluau: diagnostics,
// hover, completion and definitions through the editor's own commands.
//   node test/editor/run.js <editor program> <cluau program>
// The editor is VS Code's or VSCodium's program itself (not the `code`
// command, which hands its arguments to the program and returns); it opens a
// window for the few seconds the test takes, with a profile of its own.
// CLUAU_EDITOR_ARGS adds arguments (`--no-sandbox` where the system's sandbox
// is not available, as on CI).

"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");

const [editor, cluau] = process.argv.slice(2);
if (!editor || !cluau) {
	console.error("usage: node test/editor/run.js <editor program> <cluau program>");
	process.exit(2);
}
const root = fs.mkdtempSync(path.join(os.tmpdir(), "cluau-editor-"));
const workspace = path.join(root, "workspace");
fs.mkdirSync(path.join(workspace, ".vscode"), { recursive: true });
fs.writeFileSync(path.join(workspace, ".vscode", "settings.json"), JSON.stringify({ "cluau.path": path.resolve(cluau) }));
fs.writeFileSync(
	path.join(workspace, "shapes.cluau"),
	"// A point on the plane.\nclass Point {\n\tX: number = 0\n\n\t// How far from the origin.\n\tfunction Length(): number {\n\t\treturn self.X\n\t}\n}\n\nreturn { Point = Point }\n"
);
fs.writeFileSync(
	path.join(workspace, "main.cluau"),
	'const shapes = require("./shapes")\nconst p = shapes.Point()\nprint(p:Length(), missing)\n'
);
const env = { ...process.env, CLUAU_EDITOR_TEST_DIR: workspace };
delete env.ELECTRON_RUN_AS_NODE;
const result = spawnSync(
	editor,
	[
		workspace,
		`--extensionDevelopmentPath=${path.resolve(__dirname, "..", "..")}`,
		`--extensionTestsPath=${path.join(__dirname, "suite.js")}`,
		`--user-data-dir=${path.join(root, "user")}`,
		`--extensions-dir=${path.join(root, "extensions")}`,
		"--disable-workspace-trust",
		"--skip-welcome",
		"--skip-release-notes",
		"--disable-gpu",
		...(process.env.CLUAU_EDITOR_ARGS || "").split(" ").filter(Boolean),
	],
	{ env, stdio: "inherit", timeout: 120000 }
);
fs.rmSync(root, { recursive: true, force: true });
process.exit(result.status === null ? 1 : result.status);
