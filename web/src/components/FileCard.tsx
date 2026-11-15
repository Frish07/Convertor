import { useState } from 'react';
import { ArrowDown, ArrowUp, ChevronDown, Download, File, Play, X } from 'lucide-react';
import { errorMessage, type Key, type Locale, type Translate } from '../i18n';
import { extension, formats, isAudio, MiB, type Job, type Settings, type Tab } from '../types';
import { downloadBlob, durationOf, sizeOf } from '../utils';
import { DurationProbe, Preview } from './Preview';

interface Props {
  job: Job;
  tab: Tab;
  t: Translate;
  locale: Locale;
  busy: boolean;
  canMoveUp: boolean;
  canMoveDown: boolean;
  change: (settings: Partial<Settings>) => void;
  move: (offset: number) => void;
  remove: () => void;
  run: () => void;
  cancel: () => void;
}

const pageModes = ['split', 'extract', 'delete', 'reorder'] as const;

function formatList(options: readonly string[], t: Translate) {
  return options.map(format => <option key={format} value={format}>{t(format as Key)}</option>);
}

export function FileCard({ job, tab, t, locale, busy, canMoveUp, canMoveDown, change, move, remove, run, cancel }: Props) {
  const [preview, setPreview] = useState(false);
  const [nativeDuration, setDuration] = useState<number>();
  const { settings } = job;
  const audioOrVideo = tab === 'video' || tab === 'audio';
  const single = job.outputs.length === 1 ? job.outputs[0] : undefined;
  const canPreview = tab !== 'pdf';
  const duration = job.duration ?? nativeDuration;
  const format = settings.format;
  const audioQuality = format === 'mp3' || format === 'ogg';
  const useAudioQuality = audioQuality && settings.audioMode === 'quality';
  const quality = tab === 'image' && ['webp', 'jpg'].includes(format)
    || audioOrVideo && (format === 'flac' || useAudioQuality || !isAudio(format) && !(settings.remux && settings.resolution === 'original'));
  const live = job.status === 'working' && job.stage ? t(job.stage) : t(job.status);
  return <article className="file-card" data-status={job.status}>
    <div className="file-header">
      <div className="file-glyph"><File size={21} /></div>
      <div className="file-identity">
        <strong title={job.file.name}>{job.file.name}</strong>
        <small>{extension(job.file.name).toUpperCase()} <span>·</span> {sizeOf(job.file.size, locale)}
          {duration !== undefined && <> <span>·</span> {durationOf(duration)}</>}
        </small>
      </div>
      <span className="status" role="status">{live}{job.progress !== undefined && ' ' + job.progress + '%'}</span>
      <div className="file-actions">
        {tab === 'pdf' && <>
          <button className="icon-button" type="button" aria-label={t('up')} disabled={busy || !canMoveUp} onClick={() => move(-1)}><ArrowUp size={16} /></button>
          <button className="icon-button" type="button" aria-label={t('down')} disabled={busy || !canMoveDown} onClick={() => move(1)}><ArrowDown size={16} /></button>
        </>}
        <button className="icon-button" type="button" title={t('remove')} aria-label={t('remove')} disabled={busy} onClick={remove}><X size={17} /></button>
      </div>
    </div>

    {audioOrVideo && duration === undefined && !preview && <DurationProbe file={job.file} name={job.file.name} duration={setDuration} />}
    <fieldset className="options" disabled={busy}>
      {tab === 'pdf' ? <>
        <label>{t('pdfMode')}
          <select value={settings.pdfMode} onChange={event => change({ pdfMode: event.target.value as Settings['pdfMode'] })}>
            {pageModes.map(mode => <option value={mode} key={mode}>{t(mode)}</option>)}
          </select>
        </label>
        {settings.pdfMode !== 'split' && <label>{t('pages')}
          <input value={settings.pages} onChange={event => change({ pages: event.target.value })} placeholder="3, 1-2" aria-describedby={'pages-' + job.id} />
          <small id={'pages-' + job.id}>{t('pageHint')}</small>
        </label>}
      </> : <label>{t('format')}
        <select value={format} onChange={event => change({ format: event.target.value })}>
          {tab === 'video' ? <>
            <optgroup label={t('videoFormats')}>{formatList(formats.video, t)}</optgroup>
            <optgroup label={t('audioFormats')}>{formatList(formats.audio, t)}</optgroup>
          </> : formatList(formats[tab], t)}
        </select>
      </label>}
      {tab === 'image' && <>
        <div className="dimensions">
          <label>{t('width')}<input type="number" min="1" max="8192" placeholder="px" value={settings.width} onChange={event => change({ width: event.target.value })} /></label>
          <label>{t('height')}<input type="number" min="1" max="8192" placeholder="px" value={settings.height} onChange={event => change({ height: event.target.value })} /></label>
        </div>
        <label className="tick"><input type="checkbox" checked={settings.keepRatio} onChange={event => change({ keepRatio: event.target.checked })} />{t('keepRatio')}</label>
      </>}
      {tab === 'video' && !isAudio(format) && <>
        <label>{t('resolution')}
          <select value={settings.resolution} onChange={event => change({ resolution: event.target.value })}>
            <option value="original">{t('original')}</option>
            {[1080, 720, 480].map(height => <option value={height} key={height}>{height}p</option>)}
          </select>
        </label>
        {settings.resolution === 'original' && <label className="tick"><input type="checkbox" checked={settings.remux} onChange={event => change({ remux: event.target.checked })} />{t('remux')}</label>}
      </>}
      {audioOrVideo && audioQuality && <label>{t('audioMode')}
        <select value={settings.audioMode} onChange={event => change({ audioMode: event.target.value as Settings['audioMode'] })}>
          <option value="bitrate">{t('bitrate')}</option>
          <option value="quality">{t('quality')}</option>
        </select>
      </label>}
      {audioOrVideo && isAudio(format) && !['flac', 'wav'].includes(format) && !useAudioQuality && <label>{t('bitrate')}
        <select value={settings.bitrate} onChange={event => change({ bitrate: Number(event.target.value) })}>
          {[64, 96, 128, 160, 192, 256, 320].map(rate => <option key={rate} value={rate}>{rate} {t('kbps')}</option>)}
        </select>
      </label>}
      {quality && <label className="range">
        <span>{t(format === 'flac' ? 'lossless' : 'quality')} <output>{settings.quality}%</output></span>
        <input type="range" min="1" max="100" value={settings.quality} onChange={event => change({ quality: Number(event.target.value) })} />
      </label>}
    </fieldset>

    {(format === 'jpg' || format === 'bmp') && <p className="note">{t('jpegWhite')}</p>}
    {job.file.size > 80 * MiB && <p className="note">{t('large')}</p>}
    {job.error && <p className="inline-error" role="alert">{errorMessage(t, job.error)}</p>}
    {job.status === 'working' && job.progress !== undefined && <progress aria-label={t('working')} max="100" value={job.progress} />}

    <div className="file-footer">
      {canPreview && <button className="button quiet" type="button" aria-expanded={preview} onClick={() => setPreview(value => !value)}><ChevronDown size={16} />{t('preview')}</button>}
      <div className="actions">
        {job.status === 'working' && <button className="button quiet" type="button" onClick={cancel}><X size={16} />{t('cancel')}</button>}
        {single && <button className="button secondary" type="button" onClick={() => downloadBlob(single.name, single.blob)}><Download size={16} />{t('download')} · {sizeOf(single.blob.size, locale)}</button>}
        {job.status !== 'working' && job.status !== 'done' && <button className="button secondary" type="button" disabled={busy} onClick={run}><Play size={15} />{t('convert')}</button>}
      </div>
    </div>
    {preview && <div className="previews">
      <Preview file={job.file} name={job.file.name} label={t('original') + ' · ' + sizeOf(job.file.size, locale)} unavailable={t('previewUnavailable')} duration={setDuration} />
      {single && <Preview key={single.name} file={single.blob} name={single.name} label={t('result') + ' · ' + sizeOf(single.blob.size, locale)} unavailable={t('previewUnavailable')} />}
    </div>}
    {job.outputs.length > 1 && <details className="output-list">
      <summary>{t('outputFiles')} ({job.outputs.length})</summary>
      {job.outputs.map(item => <button type="button" className="button quiet" key={item.name} onClick={() => downloadBlob(item.name, item.blob)}>
        <Download size={15} /><span>{item.name}</span><small>{sizeOf(item.blob.size, locale)}</small>
      </button>)}
    </details>}
  </article>;
}
