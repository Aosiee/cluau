// The checks test/editor/run.js runs inside the editor.

"use strict";
const assert = require("assert");
const path = require("path");
const vscode = require("vscode");

async function until(what, read, timeout = 30000) {
	const start = Date.now();
	for (;;) {
		const value = await read();
		if (value) return value;
		if (Date.now() - start > timeout) throw new Error(`timed out waiting for ${what}`);
		await new Promise((resolve) => setTimeout(resolve, 200));
	}
}

function text(hover) {
	return hover
		.flatMap((h) => h.contents)
		.map((c) => (typeof c === "string" ? c : c.value))
		.join("\n");
}

exports.run = async function () {
	const dir = process.env.CLUAU_EDITOR_TEST_DIR;
	const uri = vscode.Uri.file(path.join(dir, "main.cluau"));
	const document = await vscode.workspace.openTextDocument(uri);
	await vscode.window.showTextDocument(document);
	assert.strictEqual(document.languageId, "cluau");

	const diagnostics = await until("diagnostics", () => {
		const list = vscode.languages.getDiagnostics(uri);
		return list.length ? list : null;
	});
	assert.strictEqual(diagnostics.length, 1);
	assert.match(diagnostics[0].message, /missing/);
	assert.strictEqual(diagnostics[0].range.start.line, 2);

	const hover = await until("hover", async () => {
		const h = await vscode.commands.executeCommand("vscode.executeHoverProvider", uri, new vscode.Position(2, 9));
		return h && h.length ? h : null;
	});
	assert.match(text(hover), /Point:Length\(\): number/);
	assert.match(text(hover), /How far from the origin/);

	const completion = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider", uri, new vscode.Position(2, 8));
	assert.ok(completion.items.some((item) => (typeof item.label === "string" ? item.label : item.label.label) === "Length"));

	const definition = await vscode.commands.executeCommand("vscode.executeDefinitionProvider", uri, new vscode.Position(2, 9));
	const target = definition[0].uri || definition[0].targetUri;
	assert.strictEqual(path.basename(target.fsPath), "shapes.cluau");

	console.log("editor: diagnostics, hover, completion and definitions work");
};
