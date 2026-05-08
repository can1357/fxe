import { spawn } from 'node:child_process';

const child = spawn('git', ['log', '--oneline', '-n', '5']);

const textDecoder = new TextDecoder('utf8');

let stdout = '';
let stderr = '';
child.stdout.setEncoding('utf8');
child.stderr.setEncoding('utf8');
child.stdout.on('data', (chunk: string | Uint8Array) => {
  stdout += typeof chunk === 'string' ? chunk : textDecoder.decode(chunk);
});
child.stderr.on('data', (chunk: string | Uint8Array) => {
  stderr += typeof chunk === 'string' ? chunk : textDecoder.decode(chunk);
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
