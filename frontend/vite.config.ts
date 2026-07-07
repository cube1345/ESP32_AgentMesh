import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    host: '0.0.0.0',
    port: 4173,
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
    port: 4174
  }
});
