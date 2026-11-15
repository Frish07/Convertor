import JSZip from 'jszip';
import { ConvertError, extension, MiB, stem, type Output } from '../types';

export async function zipOutputs(outputs: Output[]): Promise<Output> {
  if (outputs.reduce((size, output) => size + output.blob.size, 0) > 512 * MiB) {
    throw new ConvertError('zipTooLarge');
  }
  const zip = new JSZip();
  const names = new Set<string>();
  for (const output of outputs) {
    const clean = output.name.replace(/[\\/\x00-\x1f]/g, '_');
    let name = clean;
    for (let index = 2; names.has(name.toLowerCase()); index++) {
      name = stem(clean) + ' (' + index + ').' + extension(clean);
    }
    names.add(name.toLowerCase());
    zip.file(name, await output.blob.arrayBuffer());
  }
  return { name: 'convertor-results.zip', blob: await zip.generateAsync({ type: 'blob', compression: 'STORE' }) };
}
