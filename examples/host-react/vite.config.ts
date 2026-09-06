import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";

/** peerDependency 的实际检查：打包结果里每个 peer 只许出现一份拷贝。
 *  两份 React 会让编辑器里的 hooks 在宿主的渲染树里炸掉（A2-3）。 */
const SINGLETONS = ["react", "react-dom", "@xyflow/react", "three", "zustand"];

/** rollup 的模块 id 可能带虚拟前缀（\0commonjs-proxy:）、前导斜杠或反斜杠。 */
function normalizeId(id: string): string {
  const unix = id.replace(/\\/g, "/").toLowerCase();
  const at = unix.search(/[a-z]:\//);
  return at >= 0 ? unix.slice(at) : unix.replace(/^\0+/, "");
}

function noDuplicatePeers(): Plugin {
  return {
    name: "lyflow-no-duplicate-peers",
    generateBundle(_options, bundle) {
      const copies = new Map<string, Set<string>>();
      for (const chunk of Object.values(bundle)) {
        if (chunk.type !== "chunk") continue;
        for (const id of chunk.moduleIds) {
          const unix = normalizeId(id);
          for (const name of SINGLETONS) {
            const marker = `/node_modules/${name}/`;
            const at = unix.lastIndexOf(marker);
            if (at < 0) continue;
            const root = unix.slice(0, at + marker.length);
            const seen = copies.get(name) ?? new Set<string>();
            seen.add(root);
            copies.set(name, seen);
          }
        }
      }
      const dupes = [...copies].filter(([, roots]) => roots.size > 1);
      if (dupes.length > 0) {
        const detail = dupes
          .map(([name, roots]) => `${name}:\n    ${[...roots].join("\n    ")}`)
          .join("\n  ");
        this.error(`宿主与 @lyflow/editor 装到了两份同名依赖：\n  ${detail}`);
      }
      const missing = SINGLETONS.filter((name) => !copies.has(name));
      if (missing.length > 0) {
        this.warn(`没在打包结果里找到 ${missing.join("、")}，去重检查对它们没生效`);
      }
    },
  };
}

export default defineConfig({
  plugins: [react(), noDuplicatePeers()],
  resolve: {
    // 宿主与包共用同一份实例：peerDependency 的另一半保障
    dedupe: SINGLETONS,
  },
  server: {
    port: 5174,
    strictPort: true,
  },
  build: {
    target: "chrome110",
  },
});
