// Tokenizes cluau with the extension's TextMate grammar and checks the
// scopes given to a set of samples: `node test/grammar.js`. A small reader
// of the grammar's rules (match, begin/end, captures, includes), with
// JavaScript's regular expressions standing in for Oniguruma, which the
// grammar's patterns are written to be read by both.

"use strict";
const fs = require("fs");
const path = require("path");

const grammar = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "syntaxes", "cluau.tmLanguage.json"), "utf8"));

function compile(source) {
	return new RegExp(source.replace(/\\A/g, "^").replace(/\\h/g, "[0-9A-Fa-f]"), "dg");
}

function expand(patterns) {
	const out = [];
	for (const p of patterns || []) {
		if (p.include === "$self") out.push(...expand(grammar.patterns));
		else if (p.include) {
			const rule = grammar.repository[p.include.slice(1)];
			if (!rule) throw new Error(`no rule ${p.include}`);
			out.push(...(rule.patterns && !rule.match && !rule.begin ? expand(rule.patterns) : [rule]));
		} else if (p.patterns && !p.match && !p.begin) out.push(...expand(p.patterns));
		else out.push(p);
	}
	return out;
}

const topPatterns = expand(grammar.patterns);

function tokenize(text) {
	const tokens = [];
	const stack = [{ patterns: topPatterns, scopes: [grammar.scopeName], end: null }];
	const emit = (line, text, scopes) => {
		if (text.length) tokens.push({ line, text, scopes });
	};
	// The match's text split by its captures (top-level groups).
	const emitMatch = (line, m, base, captures) => {
		let at = m.index;
		const parts = [];
		if (captures)
			for (let g = 1; g < m.indices.length; g++) {
				const range = m.indices[g];
				if (!range || !captures[g]) continue;
				parts.push({ from: range[0], to: range[1], scope: captures[g].name, patterns: captures[g].patterns });
			}
		parts.sort((a, b) => a.from - b.from);
		const whole = captures && captures[0] ? [...base, captures[0].name] : base;
		for (const part of parts) {
			if (part.from < at) continue;
			emit(line, m.input.slice(at, part.from), whole);
			if (part.patterns) emitWithin(line, m.input.slice(part.from, part.to), [...whole, part.scope], expand(part.patterns));
			else emit(line, m.input.slice(part.from, part.to), [...whole, part.scope]);
			at = part.to;
		}
		emit(line, m.input.slice(at, m.index + m[0].length), whole);
	};
	// A capture's own patterns (match rules) over its text.
	const emitWithin = (line, text, scopes, patterns) => {
		let at = 0;
		while (at < text.length) {
			let best = null;
			for (const rule of patterns) {
				if (!rule.match) continue;
				const re = rule.re || (rule.re = compile(rule.match));
				re.lastIndex = at;
				const m = re.exec(text);
				if (m && m[0].length && (!best || m.index < best.m.index)) best = { m, rule };
			}
			if (!best) break;
			emit(line, text.slice(at, best.m.index), scopes);
			emit(line, best.m[0], [...scopes, best.rule.name]);
			at = best.m.index + best.m[0].length;
		}
		emit(line, text.slice(at), scopes);
	};
	text.split("\n").forEach((lineText, lineNo) => {
		let pos = 0;
		for (let guard = 0; guard < 10000; guard++) {
			const top = stack[stack.length - 1];
			let best = null;
			const consider = (re, rule, kind) => {
				re.lastIndex = pos;
				const m = re.exec(lineText);
				if (m && (!best || m.index < best.m.index)) best = { m, rule, kind };
			};
			if (top.end) consider(top.end, top.rule, "end");
			for (const rule of top.patterns) {
				if (rule.match) consider(rule.re || (rule.re = compile(rule.match)), rule, "match");
				else if (rule.begin) consider(rule.beginRe || (rule.beginRe = compile(rule.begin)), rule, "begin");
			}
			if (!best) {
				emit(lineNo, lineText.slice(pos), top.scopes);
				break;
			}
			const { m, rule, kind } = best;
			emit(lineNo, lineText.slice(pos, m.index), top.scopes);
			const next = m.index + m[0].length;
			if (kind === "end") {
				emitMatch(lineNo, m, top.outer, rule.endCaptures || rule.captures);
				stack.pop();
			} else if (kind === "match") {
				emitMatch(lineNo, m, rule.name ? [...top.scopes, rule.name] : top.scopes, rule.captures);
			} else {
				const outer = rule.name ? [...top.scopes, rule.name] : top.scopes;
				emitMatch(lineNo, m, outer, rule.beginCaptures || rule.captures);
				const end = rule.end.replace(/\\(\d)/g, (_, n) => (m[+n] || "").replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
				stack.push({
					rule,
					patterns: expand(rule.patterns),
					scopes: rule.contentName ? [...outer, rule.contentName] : outer,
					outer,
					end: compile(end),
				});
			}
			if (next === pos && kind !== "begin") {
				emit(lineNo, lineText.slice(pos, pos + 1), top.scopes);
				pos++;
			} else pos = next;
			if (pos > lineText.length) break;
			if (pos === lineText.length && kind !== "begin") {
				// One more turn: an end pattern may match at the line's end.
				const t = stack[stack.length - 1];
				if (!t.end) break;
				t.end.lastIndex = pos;
				const e = t.end.exec(lineText);
				if (!e || e.index !== pos) break;
			}
		}
	});
	return tokens;
}

// [source, the token's text, its line (0-based), a scope it must have]
const cases = [
	["#!/usr/bin/env cluau\nprint(1)", "#!/usr/bin/env cluau", 0, "comment.line.shebang.cluau"],
	["let x = 1 // a comment", "a comment", 0, "comment.line.double-slash.cluau"],
	["let a = 1\n/* a\nblock */ let b = 2", "block", 2, "comment.block.cluau"],
	["/* a\nblock */ let b = 2", "let", 1, "storage.type.cluau"],
	["x--", "--", 0, "keyword.operator.increment.cluau"],
	["let s = \"a\\n\"", "\\n", 0, "constant.character.escape.cluau"],
	["let s = `hi {name}!`", "name", 0, "source.cluau"],
	["let s = `hi {name}!`", "!", 0, "string.interpolated.cluau"],
	["let s = [[\nlong\n]]", "long", 1, "string.quoted.other.multiline.cluau"],
	["class Dog extends Animal {", "Dog", 0, "entity.name.type.class.cluau"],
	["class Dog extends Animal {", "Animal", 0, "entity.other.inherited-class.cluau"],
	["abstract class Shape {", "abstract", 0, "storage.modifier.abstract.cluau"],
	["enum Color { Red, Green }", "Color", 0, "entity.name.type.enum.cluau"],
	["function Area(): number {", "Area", 0, "entity.name.function.cluau"],
	["function Area(): number {", "number", 0, "entity.name.type.cluau"],
	["function M.helper(a: string) {", "helper", 0, "entity.name.function.cluau"],
	["function M.helper(a: string) {", "string", 0, "entity.name.type.cluau"],
	["\tsignal Collected(position: vec3)", "Collected", 0, "entity.name.function.signal.cluau"],
	["export type Point = { x: number }", "Point", 0, "entity.name.type.alias.cluau"],
	["print(type(x))", "type", 0, "support.function.builtin.cluau"],
	["static Server = {}", "Server", 0, "variable.other.property.static.cluau"],
	["\t@export Speed: number = 4", "export", 0, "entity.other.attribute-name.cluau"],
	["\t@export Speed: number = 4", "number", 0, "entity.name.type.cluau"],
	["try { f() } catch (error) { throw error }", "catch", 0, "keyword.control.cluau"],
	["try { f() } catch (error) { throw error }", "throw", 0, "keyword.control.cluau"],
	["if (a && !b || c != d) { }", "&&", 0, "keyword.operator.logical.cluau"],
	["if (a && !b || c != d) { }", "!=", 0, "keyword.operator.comparison.cluau"],
	["let q = a ~/ b", "~/", 0, "keyword.operator.arithmetic.cluau"],
	["let n = 0x1F + 1_000 + 2.5e3", "0x1F", 0, "constant.numeric.hex.cluau"],
	["let n = 0x1F + 1_000 + 2.5e3", "1_000", 0, "constant.numeric.decimal.cluau"],
	["let n = 0x1F + 1_000 + 2.5e3", "2.5e3", 0, "constant.numeric.decimal.cluau"],
	["self:Grounded()", "Grounded", 0, "entity.name.function.method.cluau"],
	["self:Grounded()", "self", 0, "variable.language.self.cluau"],
	["let v = if (ok) nil else true", "nil", 0, "constant.language.nil.cluau"],
	["let t = math.floor(x)", "math", 0, "support.constant.library.cluau"],
	["let r = count(x)", "count", 0, "entity.name.function.call.cluau"],
	// A table inside an interpolation: its } does not end the expression.
	["print(`n = { #{1, 2, 3} } items`)", "items", 0, "string.interpolated.cluau"],
	["print(`n = { #{1, 2, 3} } items`)", "3", 0, "meta.template.expression.cluau"],
	["type Fn = (number) -> boolean", "->", 0, "keyword.operator.type.function.cluau"],
	["function Map<K, V>(k: K): V { }", "K", 0, "entity.name.type.parameter.cluau"],
	["function Map<K, V>(k: K): V { }", "<", 0, "punctuation.definition.type-parameters.cluau"],
	// A string continued in the next line by a backslash at the line's end.
	["let s = \"line one \\\nline two\"\nlet after = 1", "line two", 1, "string.quoted.double.cluau"],
	["let s = \"line one \\\nline two\"\nlet after = 1", "let", 2, "storage.type.cluau"],
	["let s = \"unterminated\nlet after = 1", "let", 1, "storage.type.cluau"],
	["let n = a?.b ?? 0", "??", 0, "keyword.operator.nil.cluau"],
	["let n = a?.b ?? 0", "?", 0, "keyword.operator.optional.cluau"],
	["x ??= 1", "??=", 0, "keyword.operator.assignment.compound.cluau"],
	["switch (x) { case 1: default: }", "case", 0, "keyword.control.cluau"],
	["function* count(n: number): number { yield n }", "count", 0, "entity.name.function.cluau"],
	["function* count(n: number): number { yield n }", "yield", 0, "keyword.control.cluau"],
	["function count(n: number) { }", "count", 0, "entity.name.function.cluau"],
];

let failures = 0;
for (const [source, text, line, scope] of cases) {
	const tokens = tokenize(source).filter((t) => t.line === line && t.text.trim() === text);
	if (!tokens.some((t) => t.scopes.includes(scope))) {
		failures++;
		const found = tokens.map((t) => t.scopes.join(" ")).join(" | ") || "(no such token)";
		console.error(`FAIL ${JSON.stringify(source)}: ${JSON.stringify(text)} should be ${scope}; got ${found}`);
	}
}
console.log(`grammar: ${cases.length} case(s), ${failures} failure(s)`);
process.exit(failures ? 1 : 0);
