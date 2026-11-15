import { ConvertError, type Output } from '../types';
import type { Task, WorkerReply } from '../workers/tasks';

export function runWorker(task: Task, signal: AbortSignal): Promise<Output[]> {
  signal.throwIfAborted();
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('../workers/files.worker.ts', import.meta.url), { type: 'module' });
    function cleanup() {
      worker.terminate();
      signal.removeEventListener('abort', abort);
    }
    function abort() {
      cleanup();
      reject(signal.reason);
    }
    signal.addEventListener('abort', abort, { once: true });
    worker.onmessage = ({ data }: MessageEvent<WorkerReply>) => {
      cleanup();
      if ('error' in data) reject(new ConvertError(data.error));
      else resolve(data.outputs);
    };
    worker.onerror = () => {
      cleanup();
      reject(new ConvertError('conversionFailed'));
    };
    try { worker.postMessage(task); }
    catch (error) { cleanup(); reject(error); }
  });
}
