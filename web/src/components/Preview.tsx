import { useState, type SyntheticEvent } from 'react';
import { useBlobUrl } from '../utils';
import { extension, inputFormats } from '../types';

interface Props {
  file: Blob;
  name: string;
  label: string;
  unavailable: string;
  duration?: (seconds: number) => void;
}

export function Preview({ file, name, label, unavailable, duration }: Props) {
  const url = useBlobUrl(file);
  const [failed, setFailed] = useState(false);
  const ext = extension(name);
  const image = (inputFormats.image as readonly string[]).includes(ext);
  const video = inputFormats.video.includes(ext);
  const audio = inputFormats.audio.includes(ext) && !video;
  const metadata = (event: SyntheticEvent<HTMLMediaElement>) => {
    const seconds = event.currentTarget.duration;
    if (Number.isFinite(seconds)) duration?.(seconds);
  };
  if (!url || !(image || video || audio)) return null;
  return <figure className="preview">
    <figcaption>{label}</figcaption>
    {failed ? <p className="note">{unavailable}</p> : <>
      {image && <img src={url} alt={name} onError={() => setFailed(true)} />}
      {audio && <audio src={url} controls preload="metadata" aria-label={label} onLoadedMetadata={metadata} onError={() => setFailed(true)} />}
      {video && <video src={url} controls playsInline preload="metadata" aria-label={label} onLoadedMetadata={metadata} onError={() => setFailed(true)} />}
    </>}
  </figure>;
}

// Probe the duration via the browser without keeping dozens of players mounted in the queue.
export function DurationProbe({ file, name, duration }: Pick<Props, 'file' | 'name' | 'duration'>) {
  const url = useBlobUrl(file);
  if (!url) return null;
  const Tag = inputFormats.video.includes(extension(name)) ? 'video' : 'audio';
  return <Tag src={url} preload="metadata" hidden onLoadedMetadata={event => {
    if (Number.isFinite(event.currentTarget.duration)) duration?.(event.currentTarget.duration);
  }} />;
}
