import { expect, test, type Page, type Locator } from '@playwright/test';
import JSZip from 'jszip';
import { PDFDocument } from 'pdf-lib';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const fixture = (file: string) => fileURLToPath(new URL('./fixtures/' + file, import.meta.url));
const files = (page: Page) => page.getByTestId('files');
const cards = (page: Page) => page.locator('.file-card');
const card = (page: Page, name: string) => cards(page).filter({ hasText: name });
async function completed(item: Locator) {
  await expect(item).toHaveAttribute('data-status', /done|error/, { timeout: 110_000 });
  const error = await item.getByRole('alert').allTextContents();
  expect(error).toEqual([]);
}

async function english(page: Page) {
  await page.goto('/Convertor/');
  await page.getByRole('button', { name: 'EN', exact: true }).click();
}
async function getDownload(page: Page, trigger: Locator) {
  const pending = page.waitForEvent('download');
  await trigger.click();
  const download = await pending;
  const path = await download.path();
  return { name: download.suggestedFilename(), buffer: await readFile(path!) };
}
async function runOne(page: Page, item: Locator) {
  await item.getByRole('button', { name: 'Process file' }).click();
  await completed(item);
  return getDownload(page, item.getByRole('button', { name: /^Download/ }));
}

function wav() {
  const samples = 16000, channels = 1, rate = 16000;
  const bytes = Buffer.alloc(44 + samples * 2);
  bytes.write('RIFF', 0);
  bytes.writeUInt32LE(bytes.length - 8, 4);
  bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16);
  bytes.writeUInt16LE(1, 20);
  bytes.writeUInt16LE(channels, 22);
  bytes.writeUInt32LE(rate, 24);
  bytes.writeUInt32LE(rate * 2, 28);
  bytes.writeUInt16LE(2, 32);
  bytes.writeUInt16LE(16, 34);
  bytes.write('data', 36);
  bytes.writeUInt32LE(samples * 2, 40);
  for (let sample = 0; sample < samples; sample++) {
    bytes.writeInt16LE(Math.round(Math.sin(sample * 2 * Math.PI * 440 / rate) * 16000), 44 + sample * 2);
  }
  return bytes;
}

async function makeImage(page: Page, type: string) {
  const base64 = await page.evaluate(type => {
    const canvas = document.createElement('canvas');
    canvas.width = 36;
    canvas.height = 24;
    const context = canvas.getContext('2d')!;
    context.fillStyle = '#ff3210';
    context.fillRect(0, 0, 18, 24);
    return canvas.toDataURL(type).split(',')[1];
  }, type);
  return Buffer.from(base64, 'base64');
}

async function pixel(page: Page, image: Buffer, type: string, x: number) {
  return page.evaluate(async ({ data, type, x }) => {
    const bitmap = await createImageBitmap(new Blob([Uint8Array.from(atob(data), value => value.charCodeAt(0))], { type }));
    const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
    const context = canvas.getContext('2d')!;
    context.drawImage(bitmap, 0, 0);
    const result = { rgba: [...context.getImageData(x, 3, 1, 1).data], width: bitmap.width, height: bitmap.height };
    bitmap.close();
    return result;
  }, { data: image.toString('base64'), type, x });
}

test('base path, responsive layout, languages and theme persist', async ({ page }) => {
  const unexpected: string[] = [];
  page.on('pageerror', error => unexpected.push(error.message));
  await page.goto('/Convertor/');
  await expect(page.getByRole('heading', { level: 1 })).toContainText('Файлы остаются у вас');
  await expect(page.locator('.brand img')).toHaveJSProperty('complete', true);
  expect(await page.locator('.brand img').evaluate((image: HTMLImageElement) => image.naturalWidth)).toBeGreaterThan(0);
  await page.getByRole('button', { name: 'EN', exact: true }).click();
  await page.getByRole('button', { name: 'Toggle theme' }).click();
  await page.reload();
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await expect(page.getByRole('heading', { level: 1 })).toContainText('Keep your files');
  await page.setViewportSize({ width: 360, height: 760 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(360);
  await page.getByRole('tab', { name: 'Images' }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(page.getByRole('tab', { name: 'PDF', exact: true })).toHaveAttribute('aria-selected', 'true');
  expect(unexpected).toEqual([]);
});

test('drag-and-drop images, resize, previews, PNG/JPG/WebP/BMP and ZIP', async ({ page }) => {
  const net: string[] = [];
  page.on('request', request => {
    if (request.method() !== 'GET' || request.url().includes('ffmpeg-core')) net.push(request.url());
  });
  await english(page);
  const png = await makeImage(page, 'image/png');
  const jpg = await makeImage(page, 'image/jpeg');
  const webp = await makeImage(page, 'image/webp');
  const transfer = await page.evaluateHandle(base64 => {
    const data = new DataTransfer();
    data.items.add(new File([Uint8Array.from(atob(base64), ch => ch.charCodeAt(0))], 'source.png', { type: 'image/png' }));
    return data;
  }, png.toString('base64'));
  await page.locator('.dropzone').dispatchEvent('drop', { dataTransfer: transfer });
  await transfer.dispose();
  await files(page).setInputFiles([
    { name: 'photo.jpg', mimeType: 'image/jpeg', buffer: jpg },
    { name: 'alpha.webp', mimeType: 'image/webp', buffer: webp },
    { name: 'bitmap.png', mimeType: 'image/png', buffer: png },
  ]);
  await expect(cards(page)).toHaveCount(4);

  const first = card(page, 'source.png');
  await first.getByLabel('Output format').selectOption('jpg');
  await first.getByLabel('Width').fill('18');
  await first.getByRole('button', { name: 'Preview' }).click();
  await expect(first.locator('.preview img')).toHaveCount(1);
  await card(page, 'photo.jpg').getByLabel('Output format').selectOption('webp');
  await card(page, 'alpha.webp').getByLabel('Output format').selectOption('png');
  await card(page, 'bitmap.png').getByLabel('Output format').selectOption('bmp');
  await page.getByRole('button', { name: 'Process queue' }).click();
  await expect(page.locator('.file-card[data-status="done"]')).toHaveCount(4);
  await expect(first.locator('.preview img')).toHaveCount(2);

  const download = await getDownload(page, page.getByRole('button', { name: 'Download ZIP' }));
  expect(download.name).toBe('convertor-results.zip');
  const zip = await JSZip.loadAsync(download.buffer);
  const output = async (name: string) => zip.file(name)!.async('nodebuffer');
  const newJpg = await output('source.jpg');
  expect(newJpg.subarray(0, 2).toString('hex')).toBe('ffd8');
  const white = await pixel(page, newJpg, 'image/jpeg', 16);
  expect(white).toMatchObject({ width: 18, height: 12 });
  expect(Math.min(...white.rgba)).toBeGreaterThan(240);
  expect((await output('photo.webp')).subarray(8, 12).toString()).toBe('WEBP');
  expect((await pixel(page, await output('alpha.png'), 'image/png', 32)).rgba[3]).toBe(0);
  const bmp = await output('bitmap.bmp');
  expect(bmp.subarray(0, 2).toString()).toBe('BM');
  expect((await pixel(page, bmp, 'image/bmp', 2)).rgba).toEqual([255, 50, 16, 255]);
  await files(page).setInputFiles({ name: 'roundtrip.bmp', mimeType: 'image/bmp', buffer: bmp });
  const roundtrip = await runOne(page, card(page, 'roundtrip.bmp'));
  expect((await pixel(page, roundtrip.buffer, 'image/png', 2)).rgba).toEqual([255, 50, 16, 255]);
  expect(net).toEqual([]);
});

test('PDF merge, split, extract, delete and reorder preserve page order', async ({ page }) => {
  await english(page);
  await page.getByRole('tab', { name: 'PDF', exact: true }).click();
  const createPdf = async (widths: number[]) => {
    const pdf = await PDFDocument.create();
    for (const width of widths) pdf.addPage([width, 420]);
    return Buffer.from(await pdf.save());
  };
  const pages = async (result: Buffer) => (await PDFDocument.load(result)).getPages().map(page => page.getWidth());
  await files(page).setInputFiles([
    { name: 'first.pdf', mimeType: 'application/pdf', buffer: await createPdf([111, 222, 333]) },
    { name: 'second.pdf', mimeType: 'application/pdf', buffer: await createPdf([444]) },
  ]);
  await card(page, 'second.pdf').getByRole('button', { name: 'Move up' }).click();
  await page.getByRole('button', { name: 'Merge PDFs' }).click();
  const merge = page.locator('.merged');
  await expect(merge).toBeVisible();
  expect(await pages((await getDownload(page, merge.getByRole('button', { name: 'Download' }))).buffer)).toEqual([444, 111, 222, 333]);

  const first = card(page, 'first.pdf');
  await first.getByRole('button', { name: 'Process file' }).click();
  await completed(first);
  await first.locator('summary').click();
  const split = await getDownload(page, first.getByRole('button', { name: /first-page-2.pdf/ }));
  expect(await pages(split.buffer)).toEqual([222]);
  for (const [mode, selection, expected] of [
    ['extract', '1, 3', [111, 333]],
    ['delete', '2', [111, 333]],
    ['reorder', '3, 1-2', [333, 111, 222]],
  ] as const) {
    await first.getByLabel('PDF operation').selectOption(mode);
    await first.getByLabel('Page numbers').fill(selection);
    expect(await pages((await runOne(page, first)).buffer)).toEqual(expected);
  }
});

test('FFmpeg worker converts all audio formats, extracts MP3, and transcodes video', async ({ page }) => {
  test.setTimeout(200_000);
  await english(page);
  expect(await page.evaluate(() => crossOriginIsolated)).toBe(false);
  await page.getByRole('tab', { name: 'Audio' }).click();
  await files(page).setInputFiles({ name: 'tone.wav', mimeType: 'audio/wav', buffer: wav() });
  const tone = card(page, 'tone.wav');
  for (const [format, signature, offset] of [
    ['mp3', 'ID3', 0], ['wav', 'RIFF', 0], ['ogg', 'OggS', 0],
    ['flac', 'fLaC', 0], ['m4a', 'ftyp', 4], ['opus', 'OggS', 0],
  ] as const) {
    await tone.getByLabel('Output format').selectOption(format);
    const output = await runOne(page, tone);
    expect(output.name).toBe('tone.' + format);
    expect(output.buffer.subarray(offset, offset + signature.length).toString()).toBe(signature);
    expect(output.buffer.length).toBeGreaterThan(500);
  }
  await tone.getByLabel('Output format').selectOption('mp3');
  await tone.getByLabel('Encoding mode').selectOption('quality');
  expect((await runOne(page, tone)).buffer.subarray(0, 3).toString()).toBe('ID3');
  await tone.getByRole('button', { name: 'Preview' }).click();
  await expect.poll(() => tone.locator('.preview audio').last().evaluate((audio: HTMLAudioElement) => audio.duration)).toBeGreaterThan(0);
  await page.getByRole('tab', { name: 'Video' }).click();
  await files(page).setInputFiles([fixture('clip.mp4'), fixture('clip.webm')]);
  const mp4 = card(page, 'clip.mp4');
  const webm = card(page, 'clip.webm');
  await mp4.getByLabel('Output format').selectOption('mp3');
  expect((await runOne(page, mp4)).buffer.subarray(0, 3).toString()).toBe('ID3');
  const video = await runOne(page, webm);
  expect(video.name).toBe('clip.mp4');
  expect(video.buffer.subarray(4, 8).toString()).toBe('ftyp');
  await webm.getByRole('button', { name: 'Preview' }).click();
  await expect.poll(() => webm.locator('.preview video').last().evaluate((video: HTMLVideoElement) => video.duration)).toBeGreaterThan(0);
  await mp4.getByLabel('Output format').selectOption('webm');
  await mp4.getByLabel('Resolution').selectOption('480');
  expect((await runOne(page, mp4)).buffer.subarray(0, 4).toString('hex')).toBe('1a45dfa3');
});

test('cancelling a conversion stops the worker and a retry succeeds', async ({ page }) => {
  await english(page);
  await page.getByRole('tab', { name: 'Video' }).click();
  await files(page).setInputFiles([fixture('cancel.mp4'), fixture('clip.mp4')]);
  const large = card(page, 'cancel.mp4');
  await large.getByLabel('Output format').selectOption('webm');
  await large.getByRole('button', { name: 'Process file' }).click();
  await expect(large).toHaveAttribute('data-status', 'working');
  await expect(large.getByRole('status')).toHaveText(/^Processing…/, { timeout: 110_000 });
  await large.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(large).toHaveAttribute('data-status', 'cancelled');
  await expect(large.getByRole('button', { name: /^Download/ })).toHaveCount(0);
  const second = card(page, 'clip.mp4');
  await second.getByLabel('Output format').selectOption('mp3');
  expect((await runOne(page, second)).buffer.subarray(0, 3).toString()).toBe('ID3');
});
