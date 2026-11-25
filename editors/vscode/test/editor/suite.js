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

	// The debugger: a breakpoint in the module pauses there; the frame's
	// variables and an expression read the instance.
	const shapes = vscode.Uri.file(path.join(dir, "shapes.cluau"));
	const messages = [];
	const tracker = vscode.debug.registerDebugAdapterTrackerFactory("cluau", {
		createDebugAdapterTracker: () => ({ onDidSendMessage: (m) => messages.push(m) }),
	});
	vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(shapes, new vscode.Position(6, 0)))]);
	const ended = new Promise((resolve) => vscode.debug.onDidTerminateDebugSession(resolve));
	assert.ok(await vscode.debug.startDebugging(undefined, { type: "cluau", request: "launch", name: "test", program: uri.fsPath }));
	const stopped = await until("a pause", () => messages.find((m) => m.type === "event" && m.event === "stopped"));
	assert.strictEqual(stopped.body.reason, "breakpoint");
	const session = vscode.debug.activeDebugSession;
	const { stackFrames } = await session.customRequest("stackTrace", { threadId: 1 });
	assert.strictEqual(stackFrames[0].name, "Length");
	assert.strictEqual(stackFrames[0].line, 7);
	assert.strictEqual(path.basename(stackFrames[0].source.path), "shapes.cluau");
	const value = await session.customRequest("evaluate", { expression: "self.X + 1", frameId: stackFrames[0].id, context: "watch" });
	assert.strictEqual(value.result, "1");
	await session.customRequest("continue", { threadId: 1 });
	await ended;
	tracker.dispose();
	const exited = messages.find((m) => m.type === "event" && m.event === "exited");
	assert.ok(exited, "the session reports the script's end");
	assert.strictEqual(exited.body.exitCode, 0);
	assert.ok(messages.some((m) => m.type === "event" && m.event === "output" && /0\tnil/.test(m.body.output)), "the script's output is in the debug console");

	console.log("editor: diagnostics, hover, completion, definitions and the debugger work");
};
