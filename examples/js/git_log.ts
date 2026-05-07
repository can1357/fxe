// @ts-ignore FXE host-backed builtin
import { spawn } from 'node:child_process';

const child = spawn('git', ['log', '--oneline', '-n', '5']);

let stdout = '';
let stderr = '';
child.stdout.setEncoding('utf8');
child.stderr.setEncoding('utf8');
child.stdout.on('data', (chunk: string) => {
  stdout += String(chunk);
});
child.stderr.on('data', (chunk: string) => {
  stderr += String(chunk);
});
child.on('close', (code: number | null) => {
  if (stdout.length > 0) {
    console.log(stdout.trimEnd());
  }
  if (stderr.length > 0) {
    console.log(stderr.trimEnd());
  }
  console.log(`git log exited with code ${code}`);
});
