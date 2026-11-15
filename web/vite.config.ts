import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

export default defineConfig({
  base: '/Convertor/',
  plugins: [react(), {
    name: 'production-content-security-policy',
    apply: 'build',
    transformIndexHtml: {
      order: 'pre',
      handler: () => [{
        tag: 'meta',
        attrs: {
          'http-equiv': 'Content-Security-Policy',
          content: "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self' https://cdn.jsdelivr.net; style-src 'self'; img-src 'self' blob:; media-src blob:; worker-src 'self' blob:; object-src 'none'; base-uri 'none'; form-action 'none'",
        },
        injectTo: 'head-prepend',
      }],
    },
  }],
  worker: { format: 'es' },
});
