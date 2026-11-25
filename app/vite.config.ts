import fs from 'node:fs';
import path from 'node:path';
import { defineConfig, type Plugin } from 'vite';
import react from '@vitejs/plugin-react';

const nativeSdk =
  process.env.LYFLOW_NATIVE_SDK ?? path.resolve(import.meta.dirname, '../build/native-sdk');

// 浏览器模式（不在 Tauri 里）读 /manifest.dev.json，直接给 SDK 里的 operator-manifest.json。
function sdkManifest(): Plugin {
  return {
    name: 'lyflow-sdk-manifest',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use('/manifest.dev.json', (_req, res, next) => {
        const file = path.join(nativeSdk, 'operator-manifest.json');
        if (!fs.existsSync(file)) return next();
        res.setHeader('Content-Type', 'application/json');
        fs.createReadStream(file).pipe(res);
      });
    },
  };
}

// Tauri 从 devUrl 加载这个 dev server，端口必须与 bridge/tauri.conf.json 对上。
export default defineConfig({
  plugins: [react(), sdkManifest()],
  clearScreen: false,
  // 宿主与 @lyflow/editor 必须共用同一个 React/React Flow/three 实例，
  // 否则 hooks 与 context 会在两份副本之间对不上（A2-3）。
  resolve: {
    dedupe: ['react', 'react-dom', '@xyflow/react', 'three', 'zustand', 'qing-steel'],
  },
  server: {
    port: 5173,
    strictPort: true, // 端口被占就报错，别静默换端口让 Tauri 加载到空白页
    // tauri dev 等 cargo 编完才开窗口，这段时间先把入口的模块图转换好
    warmup: { clientFiles: ['./src/main.tsx'] },
  },
  build: {
    target: 'chrome110', // Tauri 用 WebView2，不用管老浏览器
    sourcemap: true,
  },
});
