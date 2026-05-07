// Native worker fixture. The contract test currently asserts FXE's truthful
// native-isolate boundary; real Worker support must run this file in a separate
// V8 isolate with parentPort and workerData wired by __fxe_native.worker.

// @ts-ignore FXE host-backed builtin
import { parentPort, threadId, workerData } from 'node:worker_threads';

parentPort?.postMessage({
  kind: 'fxe-worker-smoke',
  threadId,
  workerData,
});
