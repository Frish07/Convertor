import { readFile, readdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

const lock = JSON.parse(await readFile('package-lock.json', 'utf8'));
const supplemental = {
  '@ffmpeg/ffmpeg': 'ffmpeg-wasm-MIT.txt',
  '@ffmpeg/types': 'ffmpeg-wasm-MIT.txt',
  isarray: 'isarray-MIT.txt',
};
const notices = [
  'Convertor Web — Third-party licenses',
  'The separate FFmpeg WebAssembly binary is downloaded from the upstream package distributor at runtime. See third-party-licenses.html for its version, source and build instructions.',
];
for (const [path, info] of Object.entries(lock.packages)) {
  if (!path || info.dev) continue;
  const name = path.slice(path.lastIndexOf('node_modules/') + 'node_modules/'.length);
  const names = (await readdir(path)).filter(file => /^(licen[sc]e|copying|notice|copyright)/i.test(file)).sort();
  const files = names.map(file => resolve(path, file));
  if (!files.length && supplemental[name]) files.push(resolve('licenses', supplemental[name]));
  if (!files.length) throw new Error('No license text for ' + name);
  notices.push('='.repeat(70) + '\n' + name + ' ' + info.version + ' — ' + info.license);
  for (const file of files) notices.push((await readFile(file, 'utf8')).trim());
}
await writeFile('dist/third-party-licenses.txt', notices.join('\n\n') + '\n');
