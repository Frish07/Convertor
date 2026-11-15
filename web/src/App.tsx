import { useEffect, useRef, useState, type DragEvent } from 'react';
import {
  Archive, ArrowDownToLine, ArrowRight, AudioLines, CircleAlert, FileText, Film,
  Image, Laptop, Moon, Play, RefreshCw, ShieldCheck, Sun, Upload, X,
} from 'lucide-react';
import { useQueue } from './hooks/useQueue';
import { errorMessage, translate, type Locale } from './i18n';
import { inputFormats, limitFor, MiB, releaseUrl, repoUrl, type Tab } from './types';
import { downloadBlob, readPreference, savePreference } from './utils';
import { FileCard } from './components/FileCard';

const tabs = ['video', 'audio', 'image', 'pdf'] as const;
const icons = { image: Image, video: Film, audio: AudioLines, pdf: FileText };
const examples = {
  image: ['WEBP', 'PNG'], video: ['WEBM', 'MP4'], audio: ['WAV', 'MP3'], pdf: ['PDF + PDF', 'PDF'],
};
const formatLabels = {
  image: 'PNG, JPG, WebP, BMP',
  video: 'MP4, WebM, MOV, MKV, AVI…',
  audio: 'MP3, WAV, OGG, FLAC, M4A, Opus',
  pdf: 'PDF',
};

function GithubIcon() {
  return <svg viewBox="0 0 24 24" width="19" height="19" fill="currentColor" aria-hidden="true">
    <path d="M12 .75a11.25 11.25 0 0 0-3.56 21.92c.56.1.77-.24.77-.54v-2.1c-3.13.68-3.8-1.33-3.8-1.33-.5-1.3-1.25-1.64-1.25-1.64-1.02-.7.08-.69.08-.69 1.13.08 1.72 1.16 1.72 1.16 1 1.72 2.64 1.22 3.29.93.1-.73.39-1.22.71-1.5-2.5-.28-5.13-1.25-5.13-5.56 0-1.23.44-2.24 1.16-3.03-.12-.28-.5-1.43.11-2.98 0 0 .95-.3 3.1 1.16a10.8 10.8 0 0 1 5.64 0c2.15-1.46 3.1-1.16 3.1-1.16.61 1.55.23 2.7.11 2.98.72.8 1.16 1.8 1.16 3.03 0 4.32-2.63 5.27-5.14 5.55.4.35.76 1.03.76 2.08v3.1c0 .3.2.64.77.53A11.25 11.25 0 0 0 12 .75Z" />
  </svg>;
}

export function App() {
  const [locale, setLocale] = useState<Locale>(() => readPreference('convertor.lang') === 'en' ? 'en' : 'ru');
  const [theme, setTheme] = useState(() => readPreference('convertor.theme') === 'light' ? 'light' : 'dark');
  const [tab, setTab] = useState<Tab>('image');
  const [drag, setDrag] = useState(false);
  const dragDepth = useRef(0);
  const input = useRef<HTMLInputElement>(null);
  const queue = useQueue();
  const t = translate(locale);

  useEffect(() => {
    document.documentElement.lang = locale;
    savePreference('convertor.lang', locale);
  }, [locale]);
  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    savePreference('convertor.theme', theme);
  }, [theme]);
  useEffect(() => {
    const prevent = (event: globalThis.DragEvent) => {
      if (event.dataTransfer?.types.includes('Files')) event.preventDefault();
    };
    window.addEventListener('dragover', prevent);
    window.addEventListener('drop', prevent);
    return () => {
      window.removeEventListener('dragover', prevent);
      window.removeEventListener('drop', prevent);
    };
  }, []);

  const items = queue.queues[tab];
  const pending = items.filter(job => job.status !== 'done');
  const outputs = items.flatMap(job => job.outputs).concat(tab === 'pdf' && queue.merged ? [queue.merged] : []);
  const CategoryIcon = icons[tab];
  const processing = queue.running;
  const busy = Boolean(processing);
  const [from, to] = examples[tab];

  function activateTab(next: Tab) {
    setTab(next);
    dragDepth.current = 0;
    setDrag(false);
  }
  function addDrop(event: DragEvent<HTMLElement>) {
    event.preventDefault();
    dragDepth.current = 0;
    setDrag(false);
    queue.add(tab, event.dataTransfer.files);
  }

  return <>
    <header className="topbar">
      <div className="shell bar-inner">
        <a className="brand" href={import.meta.env.BASE_URL} aria-label="Convertor Web">
          <img src={import.meta.env.BASE_URL + 'convertor.ico'} alt="" />
          <span>Convertor <b>Web</b></span>
        </a>
        <div className="bar-actions">
          <div className="lang" aria-label={t('language')}>
            {(['ru', 'en'] as const).map(language => <button type="button" key={language} aria-pressed={locale === language} lang={language} onClick={() => setLocale(language)}>{language.toUpperCase()}</button>)}
          </div>
          <button type="button" className="icon-button theme-toggle" onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')} aria-label={t('theme')} title={t('theme')}>
            {theme === 'dark' ? <Sun size={18} /> : <Moon size={18} />}
          </button>
          <a className="icon-button github" href={repoUrl} aria-label={t('github')} title={t('github')}><GithubIcon /></a>
          <a className="button secondary desktop-link" href={releaseUrl}><ArrowDownToLine size={16} /><span>{t('desktop')}</span></a>
        </div>
      </div>
    </header>

    <main className="shell">
      <section className="hero">
        <div className="hero-copy">
          <div className="local"><span></span>{t('local')}</div>
          <h1>{t('headline')}<br /><em>{t('headlineAccent')}</em></h1>
          <p>{t('intro')}</p>
        </div>
        <div className="diagram" aria-hidden="true">
          <div className="format-card from"><CategoryIcon size={25} /><strong>{from}</strong></div>
          <div className="diagram-arrow"><ArrowRight size={21} /></div>
          <div className="format-card to"><CategoryIcon size={25} /><strong>{to}</strong></div>
        </div>
      </section>

      <section className="workspace" aria-label="Convertor Web">
        <div className="tabs" role="tablist" onKeyDown={event => {
          const index = tabs.indexOf(tab);
          const offsets: Record<string, number> = { ArrowLeft: (index + 3) % 4, ArrowRight: (index + 1) % 4, Home: 0, End: 3 };
          if (offsets[event.key] === undefined) return;
          event.preventDefault();
          const next = tabs[offsets[event.key]];
          activateTab(next);
          document.getElementById('tab-' + next)?.focus();
        }}>
          {tabs.map(item => {
            const Icon = icons[item];
            return <button type="button" role="tab" id={'tab-' + item} aria-controls="conversion-panel" tabIndex={tab === item ? 0 : -1}
              aria-selected={tab === item} key={item} onClick={() => activateTab(item)}><Icon size={18} />{t(item)}</button>;
          })}
        </div>
        <div className="tab-panel" id="conversion-panel" role="tabpanel" aria-labelledby={'tab-' + tab}>
          <div className={'dropzone' + (drag ? ' dragging' : '')}
            onDragEnter={event => {
              if (!event.dataTransfer.types.includes('Files')) return;
              event.preventDefault();
              dragDepth.current++;
              if (!busy) setDrag(true);
            }}
            onDragOver={event => {
              event.preventDefault();
              event.dataTransfer.dropEffect = busy ? 'none' : 'copy';
            }}
            onDragLeave={() => {
              dragDepth.current = Math.max(0, dragDepth.current - 1);
              if (!dragDepth.current) setDrag(false);
            }}
            onDrop={addDrop}>
            <input ref={input} type="file" data-testid="files" multiple accept={inputFormats[tab].map(ext => '.' + ext).join(',')} hidden
              onChange={event => {
                if (event.currentTarget.files) queue.add(tab, event.currentTarget.files);
                event.currentTarget.value = '';
              }} />
            <div className="upload-icon"><Upload size={23} strokeWidth={1.8} /></div>
            <h2>{t('drop')}</h2>
            <p>{t('dragOr')}</p>
            <button className="button primary" type="button" disabled={busy} onClick={() => input.current?.click()}><Upload size={17} />{t('choose')}</button>
            <span className="file-types">{formatLabels[tab]} <span>·</span> {limitFor(tab) / MiB} MB</span>
          </div>
          <div className="privacy"><ShieldCheck size={18} /><p>{t('privacy')}</p></div>
        </div>
      </section>

      {(queue.messages[tab]?.length ?? 0) > 0 && <div className="notice" role="status">
        <CircleAlert size={19} /><div>{queue.messages[tab]?.map(code => <p key={code}>{errorMessage(t, code)}</p>)}</div>
      </div>}

      {(items.length > 0 || processing) && <section className="queue" aria-labelledby="queue-title">
        <div className="queue-head">
          <h2 id="queue-title">{t('queue')} <span>{items.length}</span></h2>
          <div className="actions">
            {items.length > 0 && <button className="button quiet" type="button" disabled={busy} onClick={() => queue.remove(tab)}>{t('clear')}</button>}
            {outputs.length > 0 && <button className="button secondary" type="button" disabled={busy} onClick={() => void queue.sendWorker(tab, 'zip', outputs)}><Archive size={16} />{t('zip')}</button>}
            {tab === 'pdf' && items.length > 1 && <button className="button secondary" type="button" disabled={busy} onClick={() => void queue.sendWorker(tab, 'merge')}><FileText size={16} />{t('merge')}</button>}
            {busy ? <button className="button secondary" type="button" onClick={() => queue.cancel(true)}><X size={16} />{t('cancelAll')}</button> :
              items.length > 0 && <button className="button primary" type="button" disabled={!pending.length} onClick={() => void queue.process(tab, pending)}><Play size={15} />{t('run')}</button>}
          </div>
        </div>
        {processing && (processing.tab !== tab || processing.mode !== 'jobs') && <p className="queue-progress" role="status">
          <RefreshCw size={16} className="spinner" />{t(processing.mode === 'zip' ? 'packing' : 'converting')} · {t(processing.tab)}
        </p>}
        {tab === 'pdf' && items.length > 1 && <p className="note merge-note">{t('mergeHint')}</p>}
        {tab === 'pdf' && queue.merged && <div className="merged"><FileText size={18} /><strong>{t('merged')}</strong>
          <button className="button secondary" onClick={() => queue.merged && downloadBlob(queue.merged.name, queue.merged.blob)} type="button"><ArrowDownToLine size={16} />{t('download')}</button>
        </div>}
        <div className="file-list">{items.map((job, index) => <FileCard key={job.id} job={job} tab={tab} t={t} locale={locale} busy={busy}
          canMoveUp={index > 0} canMoveDown={index < items.length - 1} change={patch => queue.configure(tab, job, patch)}
          move={offset => queue.move(tab, job.id, offset)} remove={() => queue.remove(tab, job.id)}
          run={() => void queue.process(tab, [job])} cancel={() => queue.cancel(false)} />)}</div>
      </section>}

      <aside className="limits">
        <div className="limits-icon"><Laptop size={23} strokeWidth={1.75} /></div>
        <div><h2>{t('desktopTitle')}</h2><p>{t('limitations')}.</p></div>
        <a className="button secondary" href={releaseUrl}><ArrowDownToLine size={16} />{t('desktop')}</a>
      </aside>
    </main>
    <footer className="page-footer shell"><span>Convertor Web</span>
      <a href={import.meta.env.BASE_URL + 'third-party-licenses.html'}>{t('licenses')}</a>
    </footer>
  </>;
}
