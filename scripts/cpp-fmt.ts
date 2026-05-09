#!/usr/bin/env bun
// Run clang-format over tracked C/C++ sources (cached + untracked-but-not-
// gitignored). Pass --check for a dry-run that exits non-zero on diffs.

import { spawnSync } from "node:child_process";

const check = Bun.argv.includes("--check");

const ls = spawnSync(
	"git",
	[
		"ls-files",
		"-z",
		"--cached",
		"--others",
		"--exclude-standard",
		"*.cpp",
		"*.hpp",
		"*.cc",
		"*.cxx",
		"*.h",
		"*.hh",
		"*.hxx",
	],
	{ encoding: "buffer" },
);
if (ls.status !== 0) process.exit(ls.status ?? 1);

const exclude = /^(third_party|vendor|vcpkg|build|build-[^/]*|node_modules)\//;
const files = ls.stdout
	.toString("utf8")
	.split("\0")
	.filter((f) => f && !exclude.test(f));

if (files.length === 0) process.exit(0);

const args = check ? ["--dry-run", "--Werror", ...files] : ["-i", ...files];
const r = spawnSync("clang-format", args, { stdio: "inherit" });
process.exit(r.status ?? 1);
