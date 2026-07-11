import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

function readHttpsConfig() {
  const keyPath = process.env.ESPAGENT_HTTPS_KEY;
  const certPath = process.env.ESPAGENT_HTTPS_CERT;

  if (!keyPath && !certPath) {
    return undefined;
  }
  if (!keyPath || !certPath) {
    throw new Error('ESPAGENT_HTTPS_KEY and ESPAGENT_HTTPS_CERT must be set together.');
  }

  const resolvedKeyPath = resolve(keyPath);
  const resolvedCertPath = resolve(certPath);
  if (!existsSync(resolvedKeyPath) || !existsSync(resolvedCertPath)) {
    throw new Error(`HTTPS cert files not found: ${resolvedKeyPath}, ${resolvedCertPath}`);
  }

  return {
    key: readFileSync(resolvedKeyPath),
    cert: readFileSync(resolvedCertPath)
  };
}

const https = readHttpsConfig();

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    host: '0.0.0.0',
    port: 4173,
    https,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:4175',
        changeOrigin: true
      },
      '/ws': {
        target: 'ws://127.0.0.1:4175',
        changeOrigin: true,
        ws: true
      }
    }
  },
  preview: {
    host: '0.0.0.0',
    port: 4174,
    https
  }
});
