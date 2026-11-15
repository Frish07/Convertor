import { ConvertError, stem, type Job, type Output } from '../types';

const maxPixels = 24_000_000;

function dimensions(job: Job, width: number, height: number): [number, number] {
  const desiredWidth = Number(job.settings.width || 0);
  const desiredHeight = Number(job.settings.height || 0);
  const valid = (value: number) => Number.isInteger(value) && value >= 0 && value <= 8192;
  if (!valid(desiredWidth) || !valid(desiredHeight) || (job.settings.width && !desiredWidth)
    || (job.settings.height && !desiredHeight)) throw new ConvertError('invalidDimensions');
  if (!job.settings.keepRatio || (!desiredWidth && !desiredHeight)) {
    return [desiredWidth || width, desiredHeight || height];
  }
  const ratio = Math.min(desiredWidth ? desiredWidth / width : Infinity,
    desiredHeight ? desiredHeight / height : Infinity);
  return [Math.max(1, Math.round(width * ratio)), Math.max(1, Math.round(height * ratio))];
}

function encodeBmp(context: OffscreenCanvasRenderingContext2D, width: number, height: number) {
  const stride = Math.ceil(width * 3 / 4) * 4;
  const storage = new Uint8Array(54 + stride * height);
  const header = new DataView(storage.buffer);
  header.setUint16(0, 0x4d42, true);
  header.setUint32(2, storage.length, true);
  header.setUint32(10, 54, true);
  header.setUint32(14, 40, true);
  header.setInt32(18, width, true);
  header.setInt32(22, height, true);
  header.setUint16(26, 1, true);
  header.setUint16(28, 24, true);
  header.setUint32(34, stride * height, true);
  const { data } = context.getImageData(0, 0, width, height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const from = (y * width + x) * 4;
      const to = 54 + (height - y - 1) * stride + x * 3;
      storage[to] = data[from + 2];
      storage[to + 1] = data[from + 1];
      storage[to + 2] = data[from];
    }
  }
  return new Blob([storage], { type: 'image/bmp' });
}

export async function convertImage(job: Job): Promise<Output> {
  let bitmap: ImageBitmap;
  try { bitmap = await createImageBitmap(job.file); }
  catch { throw new ConvertError('invalidImage'); }
  const { format, quality } = job.settings;
  try {
    const [width, height] = dimensions(job, bitmap.width, bitmap.height);
    if (width > 8192 || height > 8192 || width * height > maxPixels) throw new ConvertError('imageTooLarge');
    const canvas = new OffscreenCanvas(width, height);
    const context = canvas.getContext('2d');
    if (!context) throw new ConvertError('canvasUnavailable');
    if (format === 'jpg' || format === 'bmp') {
      context.fillStyle = '#fff';
      context.fillRect(0, 0, width, height);
    }
    context.imageSmoothingQuality = 'high';
    context.drawImage(bitmap, 0, 0, width, height);
    const mime = format === 'jpg' ? 'image/jpeg' : 'image/' + format;
    const blob = format === 'bmp' ? encodeBmp(context, width, height) :
      await canvas.convertToBlob({ type: mime, quality: quality / 100 });
    if (blob.type !== mime) throw new ConvertError('encoderUnavailable');
    return { name: stem(job.file.name) + '.' + format, blob };
  } finally { bitmap.close(); }
}
