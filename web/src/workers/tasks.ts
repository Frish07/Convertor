import type { Job, Output } from '../types';

export type Task =
  | { kind: 'image' | 'pdf'; job: Job }
  | { kind: 'merge'; files: File[] }
  | { kind: 'zip'; outputs: Output[] };

export type WorkerReply = { outputs: Output[] } | { error: string };
