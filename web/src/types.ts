export type Tab = 'image' | 'video' | 'audio' | 'pdf';
export type Status = 'waiting' | 'working' | 'done' | 'error' | 'cancelled';
export type PdfMode = 'split' | 'extract' | 'delete' | 'reorder';
export type Stage = 'engine' | 'reading' | 'converting' | 'packing';

export interface Settings {
  format: string;
  width: string;
  height: string;
  keepRatio: boolean;
  quality: number;
  bitrate: number;
  audioMode: 'bitrate' | 'quality';
  resolution: string;
  remux: boolean;
  pdfMode: PdfMode;
  pages: string;
}
export interface Output { name: string; blob: Blob }
export interface Job {
  id: string;
  file: File;
  settings: Settings;
  status: Status;
  outputs: Output[];
  duration?: number;
  error?: string;
  stage?: Stage;
  progress?: number;
}

export interface Progress { stage: Stage; progress?: number; duration?: number }

export const formats = {
  image: ['png', 'jpg', 'webp', 'bmp'],
  audio: ['mp3', 'wav', 'ogg', 'flac', 'm4a', 'opus'],
  video: ['mp4', 'webm'],
  pdf: ['pdf'],
} as const;

const audioInputs = ['mp3', 'wav', 'ogg', 'oga', 'flac', 'm4a', 'aac', 'opus', 'aiff'];
const videoInputs = ['mp4', 'webm', 'mov', 'mkv', 'm4v', 'avi', 'ogv'];
export const inputFormats: Record<Tab, readonly string[]> = {
  image: ['png', 'jpg', 'jpeg', 'webp', 'bmp'],
  audio: [...audioInputs, ...videoInputs],
  video: videoInputs,
  pdf: ['pdf'],
};

export const releaseUrl = 'https://github.com/Frish07/Convertor/releases/latest';
export const repoUrl = 'https://github.com/Frish07/Convertor';
export const MiB = 1024 * 1024;
export const limitFor = (tab: Tab) => (tab === 'audio' || tab === 'video' ? 256 : 100) * MiB;
export const extension = (name: string) => name.includes('.') ? name.split('.').at(-1)!.toLowerCase() : '';
export const stem = (name: string) => name.replace(/\.[^.]+$/, '') || 'output';
export const isAudio = (format: string) => (formats.audio as readonly string[]).includes(format);
export const defaults = (tab: Tab): Settings => ({
  format: formats[tab][0], width: '', height: '', keepRatio: true, quality: 85, bitrate: 192, audioMode: 'bitrate',
  resolution: 'original', remux: true, pdfMode: 'split', pages: '',
});

export class ConvertError extends Error {
  constructor(readonly code: string) {
    super(code);
  }
}
