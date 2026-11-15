import type { FFmpeg } from '@ffmpeg/ffmpeg';
import { ConvertError, extension, isAudio, stem, type Job, type Output, type Progress } from '../types';

const coreBase = 'https://cdn.jsdelivr.net/npm/@ffmpeg/core@0.12.10/dist/esm/';
const coreIntegrity = {
  'ffmpeg-core.js': 'sha256-Z6SPEWRfhUOfP95PIRkELBazdLkQIGt6eiTzQuKNyuM=',
  'ffmpeg-core.wasm': 'sha256-n1eUelvVMNjwDFs/LLKjSS+qfl2CMxU0LWqGVtCmt7c=',
} as const;
let engine: FFmpeg | undefined;

interface Stream { index: number; codec_name: string; codec_type: string; pix_fmt?: string; disposition?: { attached_pic?: number } }
interface Inspection { audio?: Stream; video?: Stream; duration?: number }

const audioOptions: Record<string, [string, string, string]> = {
  mp3: ['libmp3lame', 'mp3', 'audio/mpeg'],
  wav: ['pcm_s16le', 'wav', 'audio/wav'],
  ogg: ['libvorbis', 'ogg', 'audio/ogg'],
  flac: ['flac', 'flac', 'audio/flac'],
  m4a: ['aac', 'ipod', 'audio/mp4'],
  opus: ['opus', 'opus', 'audio/ogg'],
};

async function load(signal: AbortSignal, report: (event: Progress) => void) {
  if (engine?.loaded) return engine;
  report({ stage: 'engine' });
  const { FFmpeg } = await import('@ffmpeg/ffmpeg');
  signal.throwIfAborted();
  const instance = new FFmpeg();
  engine = instance;
  const urls: string[] = [];
  const downloads = new AbortController();
  const abortDownloads = () => downloads.abort();
  signal.addEventListener('abort', abortDownloads, { once: true });
  async function fetchCore(name: keyof typeof coreIntegrity) {
    const response = await fetch(coreBase + name, {
      integrity: coreIntegrity[name], signal: downloads.signal, credentials: 'omit', referrerPolicy: 'no-referrer',
    });
    if (!response.ok) throw new ConvertError('engineFailed');
    const asset = await response.blob();
    downloads.signal.throwIfAborted();
    const type = name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript';
    const url = URL.createObjectURL(asset.slice(0, asset.size, type));
    urls.push(url);
    return url;
  }
  try {
    const [coreURL, wasmURL] = await Promise.all([fetchCore('ffmpeg-core.js'), fetchCore('ffmpeg-core.wasm')]);
    await instance.load({ coreURL, wasmURL }, { signal });
    return instance;
  } catch (error) {
    instance.terminate();
    engine = undefined;
    signal.throwIfAborted();
    throw new ConvertError('engineFailed');
  } finally {
    downloads.abort();
    signal.removeEventListener('abort', abortDownloads);
    for (const url of urls) URL.revokeObjectURL(url);
  }
}

async function inspect(ffmpeg: FFmpeg, input: string, signal: AbortSignal): Promise<Inspection> {
  const metadata = 'metadata.json';
  // Core 0.12.10 can return -1 after writing valid FFprobe output.
  await ffmpeg.ffprobe([
    '-v', 'error', '-protocol_whitelist', 'file', '-show_entries',
    'format=duration:stream=index,codec_name,codec_type,pix_fmt:stream_disposition=attached_pic',
    '-of', 'json', '-o', metadata, input,
  ], -1, { signal });
  let data: {
    streams?: Stream[]; format?: { duration?: string };
  };
  try { data = JSON.parse(await ffmpeg.readFile(metadata, 'utf8', { signal }) as string); }
  catch { signal.throwIfAborted(); throw new ConvertError('invalidMedia'); }
  if (!data.streams?.length) throw new ConvertError('invalidMedia');
  const duration = Number(data.format?.duration);
  return {
    audio: data.streams?.find(stream => stream.codec_type === 'audio'),
    video: data.streams?.find(stream => stream.codec_type === 'video' && !stream.disposition?.attached_pic),
    duration: Number.isFinite(duration) && duration > 0 ? duration : undefined,
  };
}

function plan(job: Job, metadata: Inspection) {
  const { format, bitrate, audioMode, quality, resolution, remux } = job.settings;
  const muxer = format === 'mp4' ? ['-movflags', '+faststart'] : [];
  if (isAudio(format)) {
    if (!metadata.audio) throw new ConvertError('noAudio');
    const [codec, container, mime] = audioOptions[format];
    const options = ['-map', '0:' + metadata.audio.index, '-c:a', codec];
    // The upstream WebAssembly build's libopus encoder fails on stereo input.
    if (format === 'opus') options.push('-strict', 'experimental');
    if (format === 'flac') options.push('-compression_level', String(Math.round(quality / 100 * 8)));
    else if (format !== 'wav') {
      // A common sample rate and channel layout let every listed bitrate work with low-rate inputs.
      options.push('-ar', '48000', '-ac', '2');
      if (audioMode === 'quality' && (format === 'mp3' || format === 'ogg')) {
        options.push('-q:a', String(Math.round(format === 'mp3' ? 9 - quality * .09 : quality / 10)));
      } else options.push('-b:a', bitrate + 'k');
    }
    return { options: [...options, '-f', container], mime, copy: [] as string[] };
  }
  if (!metadata.video) throw new ConvertError('noVideo');

  const mp4 = format === 'mp4';
  const maps = ['-map', '0:' + metadata.video.index];
  if (metadata.audio) maps.push('-map', '0:' + metadata.audio.index);
  const maxHeight = Number(resolution);
  const scale = resolution === 'original'
    ? 'pad=ceil(iw/2)*2:ceil(ih/2)*2'
    : "scale=-2:'max(2,trunc(min(ih," + maxHeight + ")/2)*2)'";
  const crf = Math.round(mp4 ? 32 - quality * .18 : 51 - quality * .4);
  // This single-threaded core decodes VP9, but its VP9 encoder can trap. VP8 encodes WebM reliably.
  const video = mp4 ? ['-c:v', 'libx264', '-preset', 'ultrafast'] :
    ['-c:v', 'libvpx', '-deadline', 'good', '-cpu-used', '4', '-b:v', (600 + 32 * quality) + 'k'];
  const options = [...maps, ...video, '-threads', '1', '-crf', String(crf), '-pix_fmt', 'yuv420p', '-vf', scale];
  if (metadata.audio) {
    options.push('-c:a', mp4 ? 'aac' : 'opus', '-b:a', '128k', '-ar', '48000', '-ac', '2');
    if (!mp4) options.push('-strict', 'experimental');
  }
  const compatibleVideo = mp4
    ? metadata.video.codec_name === 'h264' && metadata.video.pix_fmt === 'yuv420p'
    : ['vp8', 'vp9'].includes(metadata.video.codec_name);
  const compatibleAudio = !metadata.audio || (mp4 ? metadata.audio.codec_name === 'aac' :
    ['opus', 'vorbis'].includes(metadata.audio.codec_name));
  const copy = remux && resolution === 'original' && compatibleVideo && compatibleAudio
    ? [...maps, '-c', 'copy', ...muxer] : [];
  return { options: [...options, ...muxer], mime: mp4 ? 'video/mp4' : 'video/webm', copy };
}

export async function convertMedia(job: Job, signal: AbortSignal, report: (event: Progress) => void): Promise<Output[]> {
  const stop = () => {
    engine?.terminate();
    engine = undefined;
  };
  signal.addEventListener('abort', stop, { once: true });
  const input = 'source.' + extension(job.file.name);
  const output = 'result.' + job.settings.format;
  let loaded: FFmpeg | undefined;
  let listener: ((event: { time: number }) => void) | undefined;
  try {
    signal.throwIfAborted();
    loaded = await load(signal, report);
    report({ stage: 'reading' });
    const content = new Uint8Array(await job.file.arrayBuffer());
    signal.throwIfAborted();
    await loaded.writeFile(input, content, { signal });
    const metadata = await inspect(loaded, input, signal);
    let last = -1;
    listener = ({ time }) => {
      if (!metadata.duration || time < 0) return;
      const progress = Math.min(99, Math.floor(time / (metadata.duration * 10000)));
      if (progress > last) {
        last = progress;
        report({ stage: 'converting', progress });
      }
    };
    report({ stage: 'converting', duration: metadata.duration });
    loaded.on('progress', listener);
    const { options, mime, copy } = plan(job, metadata);
    const command = ['-y', '-xerror', '-abort_on', 'empty_output', '-protocol_whitelist', 'file', '-i', input];
    let code = copy.length ? await loaded.exec([...command, ...copy, output], -1, { signal }) : -1;
    if (code !== 0) {
      last = -1;
      report({ stage: 'converting' });
      code = await loaded.exec([...command, ...options, output], -1, { signal });
    }
    if (code !== 0) throw new ConvertError('conversionFailed');
    const bytes = await loaded.readFile(output, 'binary', { signal });
    if (typeof bytes === 'string' || !bytes.byteLength) throw new ConvertError('conversionFailed');
    return [{ name: stem(job.file.name) + '.' + job.settings.format, blob: new Blob([new Uint8Array(bytes)], { type: mime }) }];
  } catch (error) {
    if (!(error instanceof ConvertError) && !signal.aborted) stop();
    throw error;
  } finally {
    if (listener) loaded?.off('progress', listener);
    if (loaded?.loaded && !signal.aborted) {
      for (const file of [input, output, 'metadata.json']) {
        await loaded.deleteFile(file).catch(() => undefined);
      }
    }
    signal.removeEventListener('abort', stop);
  }
}
