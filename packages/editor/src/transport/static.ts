import type { CoreInfo, OperatorManifestBundle } from "../types/manifest";
import type { GraphDoc } from "../types/graph";
import type {
  CacheStats,
  ExecutionEvent,
  GraphDiagnostic,
  OutputInfo,
  RunOutputs,
  PlanNode,
} from "../types/execution";
import type {
  BackupStatus,
  CoreReloadFailed,
  LibraryRefresh,
  LibraryStatus,
  LoadedGraph,
  ManifestUpdated,
  RecentEntry,
  RecipeDirListing,
  RunOptions,
  Transport,
  TransportKind,
  Unlisten,
} from "./types";

/** 浏览器模式下所有执行相关的入口都走这里。 */
function browserOnly(what: string): never {
  throw new Error(`浏览器模式不能${what} —— 计算在 C++ 里，请用 Tauri 壳或 HttpTransport`);
}

/** 只读的静态快照：读一份 dump 出来的 manifest，什么都跑不了。
 *  没有后端时也能把编辑器渲染出来（宿主集成的第一步）。 */
export class StaticTransport implements Transport {
  readonly kind: TransportKind = "static";
  readonly manifestUrl: string;

  constructor(manifestUrl = "/manifest.dev.json") {
    this.manifestUrl = manifestUrl;
  }

  async getManifest(): Promise<OperatorManifestBundle> {
    const res = await fetch(this.manifestUrl);
    if (!res.ok) {
      throw new Error(
        `读不到 ${this.manifestUrl} (${res.status})。` +
          `静态模式需要先 dump 一份：scripts/dump-manifest.ps1`,
      );
    }
    return (await res.json()) as OperatorManifestBundle;
  }
  async getCoreInfo(): Promise<CoreInfo> {
    const m = await this.getManifest();
    return {
      version: (m.generatedBy ?? "unknown").split("/")[1] ?? "unknown",
      operatorCount: m.operators.length,
      typeCount: m.types.length,
      generation: 0,
      hotReload: false,
    };
  }
  async saveGraph(): Promise<void> {
    throw new Error("静态模式不能存盘，请用 Tauri 壳或 HttpTransport");
  }
  async loadGraph(): Promise<LoadedGraph> {
    throw new Error("静态模式不能读盘，请用 Tauri 壳或 HttpTransport");
  }
  async validateGraph(): Promise<GraphDiagnostic[]> {
    return browserOnly("做权威校验");
  }
  async planGraph(): Promise<PlanNode[]> {
    return browserOnly("编译计划");
  }
  async clearCache(): Promise<void> {
    return browserOnly("清缓存");
  }
  async cacheStats(): Promise<CacheStats> {
    return browserOnly("看缓存统计");
  }
  async runGraph(_doc: GraphDoc, _graphPath: string | null, options?: RunOptions): Promise<string> {
    // 单节点运行（docs/node-run-plan.md）同样要 core：说清楚是哪个入口被拦下的
    return browserOnly(options?.isolate?.length ? "只运行单个节点" : "运行");
  }
  async cancelRun(): Promise<void> {
    return browserOnly("运行");
  }
  async getOutputInfo(): Promise<OutputInfo[]> {
    return browserOnly("取运行结果");
  }
  async getRunOutputs(): Promise<RunOutputs> {
    return browserOnly("取运行结果");
  }
  async getOutputCloud(): Promise<ArrayBuffer> {
    return browserOnly("取运行结果");
  }
  async getOutputTensor(): Promise<ArrayBuffer> {
    return browserOnly("取运行结果");
  }
  async getOutputIndices(): Promise<ArrayBuffer> {
    return browserOnly("取运行结果");
  }
  async onExecutionEvent(_cb: (e: ExecutionEvent) => void): Promise<Unlisten> {
    // 这几个不抛：界面启动时无条件订阅，静态模式下静默给个空的取消函数
    // 比让整个编辑器挂在一个 useEffect 里强。真去点运行才会拿到上面那条错误。
    void _cb;
    return () => {};
  }
  async onManifestUpdated(_cb: (e: ManifestUpdated) => void): Promise<Unlisten> {
    void _cb;
    return () => {};
  }
  async onCoreReloadFailed(_cb: (e: CoreReloadFailed) => void): Promise<Unlisten> {
    void _cb;
    return () => {};
  }
  async getRecentFiles(): Promise<RecentEntry[]> {
    return [];
  }
  async pushRecentFile(): Promise<RecentEntry[]> {
    return [];
  }
  async writeBackup(): Promise<void> {
    /* 静态模式没有盘可写，静默跳过 —— 定时器不该每 30 秒弹一次错 */
  }
  async backupStatus(): Promise<BackupStatus> {
    return { exists: false, newer: false, backupModified: null, fileModified: null };
  }
  async readBackup(): Promise<LoadedGraph> {
    throw new Error("静态模式不能读盘，请用 Tauri 壳或 HttpTransport");
  }
  async discardBackup(): Promise<void> {
    /* 同上 */
  }
  // 配方文件（param-recipe P3.2）：静态模式只读。没有盘就没有目录可列；读按 URL 取（宿主把配方文件放在静态资源里时能看）
  async listRecipeDir(): Promise<RecipeDirListing> {
    return { exists: false, files: [] };
  }
  async readRecipeFile(path: string): Promise<string> {
    const res = await fetch(path);
    if (!res.ok) throw new Error(`读不到 ${path} (${res.status})`);
    return res.text();
  }
  async writeRecipeFile(): Promise<void> {
    throw new Error("静态模式只读：配方文件不能写，请用 Tauri 壳或 HttpTransport");
  }
  async deleteRecipeFile(): Promise<void> {
    throw new Error("静态模式只读：配方文件不能删，请用 Tauri 壳或 HttpTransport");
  }
  async renameRecipeFile(): Promise<void> {
    throw new Error("静态模式只读：配方文件不能改名，请用 Tauri 壳或 HttpTransport");
  }
  async writeFileBytes(): Promise<void> {
    throw new Error("静态模式不能写盘，请用 Tauri 壳或 HttpTransport");
  }
  async getLibraryStatus(): Promise<LibraryStatus> {
    return { dirs: [], count: 0, problems: [] };
  }
  async refreshLibrary(): Promise<LibraryRefresh> {
    return browserOnly("刷新库算子");
  }
  async saveAsLibrary(): Promise<LibraryStatus> {
    return browserOnly("保存到库");
  }
  async importGraph(): Promise<GraphDoc> {
    return browserOnly("导入");
  }
}
