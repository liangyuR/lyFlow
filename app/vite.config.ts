import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Tauri 从 devUrl 加载这个 dev server，端口必须与 bridge/tauri.conf.json 对上。
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  // 宿主与 @lyflow/editor 必须共用同一个 React/React Flow/three 实例，
  // 否则 hooks 与 context 会在两份副本之间对不上（A2-3）。
  resolve: {
    dedupe: ["react", "react-dom", "@xyflow/react", "three", "zustand"],
  },
  server: {
    port: 5173,
    strictPort: true,   // 端口被占就报错，别静默换端口让 Tauri 加载到空白页
  },
  build: {
    target: "chrome110",  // Tauri 用 WebView2，不用管老浏览器
    sourcemap: true,
  },
});
