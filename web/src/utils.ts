import { useEffect, useState } from 'react';

export function readPreference(key: string) {
  try { return localStorage.getItem(key); } catch { return null; }
}

export function savePreference(key: string, value: string) {
  try { localStorage.setItem(key, value); } catch { /* Browsers may disable storage. */ }
}

export function sizeOf(bytes: number, locale: string) {
  const group = bytes >= 1048576 ? [1048576, 'MB'] as const : bytes >= 1024 ? [1024, 'kB'] as const : [1, 'B'] as const;
  return new Intl.NumberFormat(locale, { maximumFractionDigits: 1 }).format(bytes / group[0]) + ' ' + group[1];
}

export function durationOf(seconds: number) {
  const duration = Math.max(0, Math.floor(seconds));
  const minutes = Math.floor(duration / 60);
  return minutes + ':' + String(duration % 60).padStart(2, '0');
}

export function useBlobUrl(blob: Blob) {
  const [url, setUrl] = useState('');
  useEffect(() => {
    const next = URL.createObjectURL(blob);
    setUrl(next);
    return () => URL.revokeObjectURL(next);
  }, [blob]);
  return url;
}

export function downloadBlob(name: string, blob: Blob) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = name;
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30_000);
}
