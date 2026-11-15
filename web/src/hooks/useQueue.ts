import { useRef, useState } from 'react';
import { runWorker } from '../converters/worker';
import {
  ConvertError, defaults, extension, inputFormats, limitFor, MiB,
  type Job, type Output, type Settings, type Tab,
} from '../types';
import { downloadBlob } from '../utils';

type Running = { tab: Tab; mode: 'jobs' | 'merge' | 'zip' };
type Active = { controller: AbortController; current?: AbortController; stop: boolean };

export function useQueue() {
  const [queues, setQueues] = useState<Record<Tab, Job[]>>({ image: [], video: [], audio: [], pdf: [] });
  const [messages, setMessages] = useState<Partial<Record<Tab, string[]>>>({});
  const [running, setRunning] = useState<Running>();
  const [merged, setMerged] = useState<Output>();
  const active = useRef<Active | undefined>(undefined);

  function change(tab: Tab, update: (items: Job[]) => Job[]) {
    setQueues(previous => ({ ...previous, [tab]: update(previous[tab]) }));
  }
  function updateJob(tab: Tab, id: string, patch: Partial<Job>) {
    change(tab, items => items.map(item => item.id === id ? { ...item, ...patch } : item));
  }
  function tell(tab: Tab, errors: string[]) {
    setMessages(previous => ({ ...previous, [tab]: errors }));
  }
  function add(tab: Tab, files: Iterable<File>) {
    if (active.current) return;
    const errors = new Set<string>();
    const added: Job[] = [];
    for (const file of files) {
      if (!inputFormats[tab].includes(extension(file.name))) errors.add('unsupported');
      else if (!file.size) errors.add('emptyFile');
      else if (file.size > limitFor(tab)) errors.add('tooLarge');
      else if (queues[tab].length + added.length >= 32) errors.add('tooManyFiles');
      else added.push({ id: crypto.randomUUID(), file, settings: defaults(tab), status: 'waiting', outputs: [] });
    }
    change(tab, items => [...items, ...added].slice(0, 32));
    if (tab === 'pdf' && added.length) setMerged(undefined);
    tell(tab, [...errors]);
  }
  function configure(tab: Tab, job: Job, settings: Partial<Settings>) {
    if (active.current) return;
    updateJob(tab, job.id, {
      settings: { ...job.settings, ...settings }, status: 'waiting', error: undefined,
      outputs: [], stage: undefined, progress: undefined,
    });
  }
  function remove(tab: Tab, id?: string) {
    if (active.current) return;
    change(tab, items => id ? items.filter(job => job.id !== id) : []);
    if (tab === 'pdf') setMerged(undefined);
    tell(tab, []);
  }
  function move(tab: Tab, id: string, offset: number) {
    if (active.current) return;
    change(tab, items => {
      const index = items.findIndex(job => job.id === id);
      const other = index + offset;
      if (index < 0 || other < 0 || other >= items.length) return items;
      const order = items.slice();
      [order[index], order[other]] = [order[other], order[index]];
      return order;
    });
    if (tab === 'pdf') setMerged(undefined);
  }
  function start(tab: Tab, mode: Running['mode']) {
    if (active.current) return undefined;
    const operation: Active = { controller: new AbortController(), stop: false };
    active.current = operation;
    setRunning({ tab, mode });
    tell(tab, []);
    return operation;
  }
  function cancel(all: boolean) {
    const operation = active.current;
    if (!operation) return;
    if (all) {
      operation.stop = true;
      operation.controller.abort();
    }
    operation.current?.abort();
  }
  function finish() {
    active.current = undefined;
    setRunning(undefined);
  }
  async function process(tab: Tab, jobs: Job[]) {
    if (!jobs.length) return;
    const operation = start(tab, 'jobs');
    if (!operation) return;
    try {
      for (const job of jobs) {
        if (operation.stop) break;
        const controller = new AbortController();
        operation.current = controller;
        updateJob(tab, job.id, {
          status: 'working', outputs: [], error: undefined, progress: undefined, stage: 'reading',
        });
        try {
          const signal = controller.signal;
          const outputs = tab === 'audio' || tab === 'video'
            ? await (await import('../converters/media')).convertMedia(job, signal, event => {
              if (signal.aborted) return;
              updateJob(tab, job.id, {
                stage: event.stage, progress: event.progress,
                ...(event.duration === undefined ? {} : { duration: event.duration }),
              });
            })
            : await runWorker({ kind: tab, job }, signal);
          signal.throwIfAborted();
          updateJob(tab, job.id, { status: 'done', outputs, progress: undefined, stage: undefined });
        } catch (error) {
          updateJob(tab, job.id, {
            status: controller.signal.aborted ? 'cancelled' : 'error',
            error: controller.signal.aborted ? undefined : error instanceof ConvertError ? error.code : 'conversionFailed',
            progress: undefined, stage: undefined,
          });
        } finally {
          operation.current = undefined;
        }
      }
    } finally { finish(); }
  }
  async function sendWorker(tab: Tab, mode: 'merge' | 'zip', outputs: Output[] = []) {
    const operation = start(tab, mode);
    if (!operation) return;
    try {
      const { signal } = operation.controller;
      if (mode === 'merge') {
        const files = queues.pdf.map(job => job.file);
        if (files.reduce((size, file) => size + file.size, 0) > 150 * MiB) throw new ConvertError('mergeTooLarge');
        const [output] = await runWorker({ kind: 'merge', files }, signal);
        signal.throwIfAborted();
        setMerged(output);
      } else {
        const [output] = await runWorker({ kind: 'zip', outputs }, signal);
        signal.throwIfAborted();
        downloadBlob(output.name, output.blob);
      }
    } catch (error) {
      tell(tab, [operation.controller.signal.aborted ? 'cancelled' :
        error instanceof ConvertError ? error.code : 'conversionFailed']);
    } finally { finish(); }
  }

  return { queues, messages, running, merged, add, configure, remove, move, process, cancel, sendWorker };
}
