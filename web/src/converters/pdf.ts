import { PDFDocument } from 'pdf-lib';
import { ConvertError, stem, type Job, type Output } from '../types';

async function load(file: File) {
  try { return await PDFDocument.load(await file.arrayBuffer()); }
  catch { throw new ConvertError('invalidPdf'); }
}

function pagesFrom(text: string, pageCount: number): number[] {
  const result = new Set<number>();
  for (const token of text.trim().split(/[,;]/)) {
    const match = /^(\d+)\s*(?:-\s*(\d+))?$/.exec(token.trim());
    if (!match) throw new ConvertError('invalidPages');
    const first = Number(match[1]), last = Number(match[2] ?? match[1]);
    if (first < 1 || last < first || last > pageCount) throw new ConvertError('invalidPages');
    for (let page = first; page <= last; page++) {
      if (result.has(page - 1)) throw new ConvertError('invalidPages');
      result.add(page - 1);
    }
  }
  return [...result];
}

async function append(target: PDFDocument, source: PDFDocument, pages: number[]) {
  for (const page of await target.copyPages(source, pages)) target.addPage(page);
}

async function output(name: string, pdf: PDFDocument): Promise<Output> {
  return { name, blob: new Blob([new Uint8Array(await pdf.save())], { type: 'application/pdf' }) };
}

export async function mergePdfs(files: File[]) {
  const merged = await PDFDocument.create();
  for (const file of files) {
    const document = await load(file);
    await append(merged, document, document.getPageIndices());
  }
  return output('merged.pdf', merged);
}

export async function convertPdf(job: Job): Promise<Output[]> {
  const source = await load(job.file);
  const name = stem(job.file.name);
  const { pdfMode, pages } = job.settings;
  const count = source.getPageCount();
  if (!count) throw new ConvertError('invalidPdf');
  if (pdfMode === 'split') {
    if (count > 500) throw new ConvertError('tooManyPages');
    const outputs = [];
    for (const page of source.getPageIndices()) {
      const single = await PDFDocument.create();
      await append(single, source, [page]);
      outputs.push(await output(name + '-page-' + (page + 1) + '.pdf', single));
    }
    return outputs;
  }
  const selected = pagesFrom(pages, count);
  if (pdfMode === 'reorder' && selected.length !== count) throw new ConvertError('allPages');
  const keep = pdfMode === 'delete' ? source.getPageIndices().filter(page => !selected.includes(page)) : selected;
  if (!keep.length) throw new ConvertError('emptyPdf');
  const result = await PDFDocument.create();
  await append(result, source, keep);
  return [await output(name + '-' + pdfMode + '.pdf', result)];
}
