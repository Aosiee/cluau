// The cluau extension: the language server (`cluau lsp`) for .cluau files,
// the debugger (`cluau debug`), and a command that runs the file in a
// terminal.

"use strict";
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind, State } = require("vscode-languageclient/node");

let client = null;
let output = null;

function workspaceDir() {
	const folders = vscode.workspace.workspaceFolders;
	return folders && folders.length ? folders[0].uri.fsPath : undefined;
}

// A setting's path, the same for the language server, the debugger and
// Run File: ${workspaceFolder} replaced, and a relative path taken from
// the workspace's folder. A program's bare name ("cluau") is looked up on
// PATH. Undefined for a relative path when no folder is open.
function resolvePath(value, program) {
	const dir = workspaceDir();
	const out = value.replace(/\$\{workspaceFolder\}/g, dir || "");
	if (path.isAbsolute(out)) return out;
	if (program && !/[\\/]/.test(out)) return out;
	return dir ? path.resolve(dir, out) : undefined;
}

function settings() {
	const config = vscode.workspace.getConfiguration("cluau");
	const configured = config.get("path") || "cluau";
	const modules = {};
	for (const [name, file] of Object.entries(config.get("modules") || {})) {
		const resolved = resolvePath(String(file));
		if (resolved) modules[name] = resolved;
		else if (output) output.appendLine(`cluau.modules: ${name} names a relative path (${file}) and no folder is open; it is left out`);
	}
	return { program: resolvePath(configured, true) || configured, modules };
}

async function start() {
	const { program, modules } = settings();
	const cwd = workspaceDir();
	const args = ["lsp"];
	for (const [name, file] of Object.entries(modules)) args.push("--module", `${name}=${file}`);
	const server = { command: program, args, transport: TransportKind.stdio, options: { cwd } };
	client = new LanguageClient(
		"cluau",
		"cluau",
		{ run: server, debug: server },
		{
			documentSelector: [{ scheme: "file", language: "cluau" }],
			synchronize: {
				// A required module changed on disk: what requires it checks again.
				fileEvents: vscode.workspace.createFileSystemWatcher("**/*.{cluau,luau}"),
			},
			outputChannel: output,
		}
	);
	try {
		await client.start();
	} catch (error) {
		client = null;
		const install = "How to install";
		const choice = await vscode.window.showErrorMessage(
			`The cluau language server did not start (${program} lsp): ${error.message}. ` +
				"It needs cluau 0.17.0 or later on PATH, or the cluau.path setting.",
			install
		);
		if (choice === install) vscode.env.openExternal(vscode.Uri.parse("https://github.com/Aosiee/cluau#installing"));
	}
}

async function stop() {
	if (client && client.state !== State.Stopped) await client.stop();
	client = null;
}

// Run File: cluau itself is the terminal's program (no shell reads the
// paths, so nothing in them needs quoting), in the file's directory. A
// terminal of an earlier run that has ended makes way; one still running
// is left alone and the run gets a terminal of its own.
async function runFile() {
	const editor = vscode.window.activeTextEditor;
	if (!editor || editor.document.languageId !== "cluau") return;
	if (editor.document.isDirty) await editor.document.save();
	const file = editor.document.uri.fsPath;
	for (const t of vscode.window.terminals) if (t.name === "cluau" && t.exitStatus !== undefined) t.dispose();
	const { program } = settings();
	const terminal = vscode.window.createTerminal({ name: "cluau", shellPath: program, shellArgs: [file], cwd: path.dirname(file) });
	terminal.show(true);
}

// F5 with no launch.json, or a configuration without a program: the
// current file, in its own directory.
const debugConfigurations = {
	resolveDebugConfiguration(folder, config) {
		if (!config.type && !config.request && !config.name) {
			const editor = vscode.window.activeTextEditor;
			if (!editor || editor.document.languageId !== "cluau") return config;
			config.type = "cluau";
			config.request = "launch";
			config.name = "Debug the current file";
			config.program = "${file}";
		}
		return config;
	},
	resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
		if (!config.program) {
			vscode.window.showErrorMessage('The cluau launch configuration names no "program" (the script to debug).');
			return undefined;
		}
		if (!config.cwd) config.cwd = path.dirname(config.program);
		// A test file's cases, as `cluau test` runs them.
		if (config.test === undefined && config.program.endsWith("_test.cluau")) config.test = true;
		return config;
	},
};

const debugAdapters = {
	createDebugAdapterDescriptor() {
		return new vscode.DebugAdapterExecutable(settings().program, ["debug"], { cwd: workspaceDir() });
	},
};

async function debugFile() {
	const editor = vscode.window.activeTextEditor;
	if (!editor || editor.document.languageId !== "cluau") return;
	if (editor.document.isDirty) await editor.document.save();
	const file = editor.document.uri.fsPath;
	await vscode.debug.startDebugging(vscode.workspace.getWorkspaceFolder(editor.document.uri), {
		type: "cluau",
		request: "launch",
		name: `Debug ${path.basename(file)}`,
		program: file,
	});
}

async function activate(context) {
	output = vscode.window.createOutputChannel("cluau");
	context.subscriptions.push(output);
	context.subscriptions.push(vscode.commands.registerCommand("cluau.run", runFile));
	context.subscriptions.push(vscode.commands.registerCommand("cluau.debug", debugFile));
	context.subscriptions.push(vscode.debug.registerDebugConfigurationProvider("cluau", debugConfigurations));
	context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory("cluau", debugAdapters));
	context.subscriptions.push(
		vscode.commands.registerCommand("cluau.restartServer", async () => {
			await stop();
			await start();
		})
	);
	context.subscriptions.push(
		vscode.workspace.onDidChangeConfiguration(async (event) => {
			if (event.affectsConfiguration("cluau.path") || event.affectsConfiguration("cluau.modules")) {
				await stop();
				await start();
			}
		})
	);
	await start();
}

async function deactivate() {
	await stop();
}

module.exports = { activate, deactivate };
