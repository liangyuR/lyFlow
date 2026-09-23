// 让 node --test 直接跑 src/ 里的 .ts：源码按 bundler 解析写的是无扩展名的相对 import，
// Node 的 ESM 解析器不补扩展名。这里只补 `.ts` / `/index.ts`，类型擦除交给 Node 自带的
// type stripping（Node >= 22.18 / 23.6 默认开启）。零依赖，免得为几条单测引 vitest。
import { existsSync } from "node:fs";
import { registerHooks } from "node:module";
import { fileURLToPath } from "node:url";

registerHooks({
  resolve(specifier, context, nextResolve) {
    const relative = specifier.startsWith("./") || specifier.startsWith("../");
    if (relative && context.parentURL?.endsWith(".ts") && !/\.[cm]?[jt]sx?$/.test(specifier)) {
      for (const suffix of [".ts", ".tsx", "/index.ts"]) {
        const url = new URL(specifier + suffix, context.parentURL);
        if (existsSync(fileURLToPath(url))) return nextResolve(url.href, context);
      }
    }
    return nextResolve(specifier, context);
  },
});
