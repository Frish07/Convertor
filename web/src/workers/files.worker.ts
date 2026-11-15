import { ConvertError, type Output } from '../types';
import type { Task, WorkerReply } from './tasks';

self.onmessage = async ({ data }: MessageEvent<Task>) => {
  let reply: WorkerReply;
  try {
    let outputs: Output[];
    if (data.kind === 'image') outputs = [await (await import('../converters/image')).convertImage(data.job)];
    else if (data.kind === 'zip') outputs = [await (await import('../converters/zip')).zipOutputs(data.outputs)];
    else {
      const pdf = await import('../converters/pdf');
      outputs = data.kind === 'merge' ? [await pdf.mergePdfs(data.files)] : await pdf.convertPdf(data.job);
    }
    reply = { outputs };
  } catch (error) {
    reply = { error: error instanceof ConvertError ? error.code : 'conversionFailed' };
  }
  self.postMessage(reply);
};
