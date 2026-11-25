// The cluau extension: the language server (`cluau lsp`) for .cluau files,
// the debugger (`cluau debug`), and a command that runs the file in a
// terminal.

"use strict";
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind, State } = require("vscode-languageclient/node");

let client = null;
let output = null;

function settings() {
	const config = vscode.workspace.getConfiguration("cluau");
	return {
		program: config.get("path") || "cluau",
		modules: config.get("modules") || {},
	};
}

function workspaceDir() {
	const folders = vscode.workspace.workspaceFolders;
	return folders && folders.length ? folders[0].uri.fsPath : undefined;
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

function quote(text) {
	if (process.platform === "win32") return `& "${text.replace(/"/g, '""')}"`;
	return `'${text.replace(/'/g, "'\\''")}'`;
}

async function runFile() {
	const editor = vscode.window.activeTextEditor;
	if (!editor || editor.document.languageId !== "cluau") return;
	if (editor.document.isDirty) await editor.document.save();
	const file = editor.document.uri.fsPath;
	const terminal = vscode.window.terminals.find((t) => t.name === "cluau") || vscode.window.createTerminal({ name: "cluau", cwd: path.dirname(file) });
	terminal.show(true);
	const { program } = settings();
	const target = process.platform === "win32" ? `"${file.replace(/"/g, '""')}"` : quote(file);
	terminal.sendText(`${quote(program)} ${target}`);
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
		return new vscode.DebugAdapterExecutable(settings().program, ["debug"]);
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
