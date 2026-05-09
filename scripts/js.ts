#!/usr/bin/env bun
// fxe launcher: build (if needed) and run a JS/TS example through fxe_run.
//
// Usage: bun scripts/js.ts [--dev] [--rebuild] [<name>] [fxe_run args...]
//
// Launcher flags (consumed; not forwarded):
//   --dev    Use the `dev` preset (default: `release`).
//   --rebuild  Force `cmake --build` even when build/<preset>/fxe_run exists.
//
// First non-flag positional is the script name (default: `ui_demo`). Resolved
// against `examples/js/<name>.{ts,tsx,mts,cts,mjs,jsx,js}` or used as-is when
// the path exists. All other args pass through to fxe_run before the script.

import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "..");
const EXTS = ["ts", "tsx", "mts", "cts", "mjs", "jsx", "js"];
const DEFAULT_NAME = "ui_demo";
const WIN = process.platform === "win32";

function run(cmd: string, args: string[]): void {
	const r = spawnSync(cmd, args, { stdio: "inherit", cwd: ROOT });
	if (r.status !== 0) process.exit(r.status ?? 1);
}

function resolveScript(name: string): string | null {
	if (existsSync(name)) return name;
	const base = join(ROOT, "examples", "js", name);
	if (existsSync(base)) return base;
	for (const ext of EXTS) {
		const p = `${base}.${ext}`;
		if (existsSync(p)) return p;
	}
	return null;
}

function ensureBuilt(preset: string, force: boolean): void {
	const binary = join(ROOT, "build", preset, WIN ? "fxe_run.exe" : "fxe_run");
	if (existsSync(binary) && !force) return;

	const vcpkg = join(ROOT, "vcpkg", WIN ? "vcpkg.exe" : "vcpkg");
	if (!existsSync(vcpkg)) {
		if (!existsSync(join(ROOT, "vcpkg", ".git"))) {
			run("git", ["clone", "--depth=1", "https://github.com/microsoft/vcpkg.git", "vcpkg"]);
		}
		run(WIN ? "vcpkg\\bootstrap-vcpkg.bat" : "./vcpkg/bootstrap-vcpkg.sh", ["-disableMetrics"]);
	}
	if (!existsSync(join(ROOT, "build", preset, "CMakeCache.txt"))) {
		run("cmake", ["--preset", preset]);
	}
	run("cmake", ["--build", "--preset", preset, "--target", "fxe_run"]);
}

let preset = "release";
let force = false;
let name: string | null = null;
const passthrough: string[] = [];
for (const arg of Bun.argv.slice(2)) {
	if (arg === "--dev") preset = "dev";
	else if (arg === "--rebuild") force = true;
	else if (name === null && !arg.startsWith("-")) name = arg;
	else passthrough.push(arg);
}
name ??= DEFAULT_NAME;

ensureBuilt(preset, force);

const binary = join(ROOT, "build", preset, WIN ? "fxe_run.exe" : "fxe_run");
if (!existsSync(binary)) {
	process.stderr.write(`fxe: ${binary} is missing after build\n`);
	process.exit(1);
}

const script = resolveScript(name);
if (!script) {
	const tried = EXTS.map((e) => `examples/js/${name}.${e}`).join(", ");
	process.stderr.write(`fxe: no script found for '${name}' (tried ${name}, ${tried})\n`);
	process.exit(1);
}

const args = [...passthrough, script];
console.log([binary, ...args].join(" "));
const r = spawnSync(binary, args, { stdio: "inherit", cwd: ROOT });
process.exit(r.status ?? 1);
