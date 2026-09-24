export interface SampleSelector {
  samplesPath?: string | undefined;
  samplesGlob?: string | undefined;
  bind?: string | undefined;
  samplesDir?: string | undefined;
  bindPair?: string | undefined;
  pattern?: string | undefined;
  sampleSubdir?: string | undefined;
  sortBy?: "name" | "mtime" | undefined;
  splitHalf?: string | undefined;
}

export interface EvalInput extends SampleSelector {
  graphPath: string;
  params?: Record<string, unknown>[] | undefined;
  param?: string[] | undefined;
  metric: string[];
  holdout?: string | undefined;
  groupBy?: string | undefined;
  csv?: string | undefined;
  baseDir?: string | undefined;
  set?: string[] | undefined;
  /// 配方文件路径（param-recipe P4）：作用于所有样本，叠在基础之上、参数组与 param 之下。
  recipe?: string | undefined;
  noCache?: boolean | undefined;
  /// 每行 eval_row 带一份 run summary（ADR-0022）。默认关：体积是逐行的。
  summary?: boolean | undefined;
}

export interface PerturbInput extends SampleSelector {
  graphPath: string;
  after: string;
  region: unknown;
  axis: string;
  metric: string[];
  expect?: number | undefined;
  tolerance?: number | undefined;
  csv?: string | undefined;
  baseDir?: string | undefined;
  set?: string[] | undefined;
  noCache?: boolean | undefined;
}

const DIR_ONLY: (keyof SampleSelector)[] = [
  "bindPair",
  "pattern",
  "sampleSubdir",
  "sortBy",
  "splitHalf",
];

function samplesArgv(input: SampleSelector): string[] {
  const given = [input.samplesPath, input.samplesGlob, input.samplesDir].filter(Boolean).length;
  if (given > 1) {
    throw new Error("samplesPath / samplesGlob / samplesDir 只能给一个");
  }
  if (!input.samplesDir) {
    for (const key of DIR_ONLY) {
      if (input[key]) throw new Error(`${key} 要和 samplesDir 一起给`);
    }
  }
  if (input.samplesPath) {
    if (input.bind) throw new Error("bind 只跟 samplesGlob / samplesDir 配套");
    return ["--samples", input.samplesPath];
  }
  if (input.samplesGlob) {
    if (!input.bind) throw new Error("samplesGlob 要配一个 bind（<节点>.<参数>）");
    return ["--samples-glob", input.samplesGlob, "--bind", input.bind];
  }
  if (input.samplesDir) {
    if (input.bindPair && input.bind) throw new Error("bindPair 与 bind 只能给一个");
    if (!input.bindPair && !input.bind) {
      throw new Error("samplesDir 要配 bindPair（双相机）或 bind（单文件）");
    }
    if (!input.pattern) {
      throw new Error("samplesDir 要配 pattern（两个相机时用逗号隔开两个 glob）");
    }
    const argv = ["--samples-dir", input.samplesDir];
    if (input.bindPair) argv.push("--bind-pair", input.bindPair);
    else if (input.bind) argv.push("--bind", input.bind);
    argv.push("--pattern", input.pattern);
    if (input.sampleSubdir) argv.push("--sample-subdir", input.sampleSubdir);
    if (input.sortBy) argv.push("--sort-by", input.sortBy);
    if (input.splitHalf) argv.push("--split-half", input.splitHalf);
    return argv;
  }
  if (input.bind) throw new Error("bind 要和 samplesGlob 或 samplesDir 一起给");
  return [];
}

export function evalArgv(input: EvalInput, paramsFile: string | null): string[] {
  if (!input.metric || input.metric.length === 0) {
    throw new Error("至少给一个 metric，例如 outputs.gap");
  }
  const argv = ["eval", input.graphPath];
  if (input.baseDir) argv.push("--base-dir", input.baseDir);
  argv.push(...samplesArgv(input));
  if (paramsFile) argv.push("--params", paramsFile);
  for (const p of input.param ?? []) argv.push("--param", p);
  for (const m of input.metric) argv.push("--metric", m);
  if (input.holdout) argv.push("--holdout", input.holdout);
  if (input.groupBy) argv.push("--group-by", input.groupBy);
  if (input.csv) argv.push("--csv", input.csv);
  for (const s of input.set ?? []) argv.push("--set", s);
  if (input.recipe) argv.push("--recipe", input.recipe);
  if (input.noCache) argv.push("--no-cache");
  if (input.summary) argv.push("--summary");
  return argv;
}

export interface ParamsInput {
  graphPath: string;
  node?: string[] | undefined;
  only?: "explicit" | "default" | "bound" | "graph" | undefined;
  set?: string[] | undefined;
  /// 配方文件路径（param-recipe P4）：图参数取「default ← 配方」之后的值。
  recipe?: string | undefined;
  baseDir?: string | undefined;
}

export function paramsArgv(input: ParamsInput): string[] {
  const argv = ["params", input.graphPath];
  if (input.baseDir) argv.push("--base-dir", input.baseDir);
  for (const n of input.node ?? []) argv.push("--node", n);
  if (input.only) argv.push("--only", input.only);
  for (const s of input.set ?? []) argv.push("--set", s);
  if (input.recipe) argv.push("--recipe", input.recipe);
  argv.push("--json");
  return argv;
}

/** `lyflow recipes <graph> [--recipe <文件>] --json`：list_recipes 列目录，run_graph 带 recipe 时
 *  拿它查失配、取合成好的图参数（一份实现在 bridge/src/recipe.rs，MCP 不再写第三份）。 */
export function recipesArgv(graphPath: string, recipe?: string | undefined): string[] {
  const argv = ["recipes", graphPath];
  if (recipe) argv.push("--recipe", recipe);
  argv.push("--json");
  return argv;
}

export interface PatchInput {
  graphPath: string;
  removeNode?: string[] | undefined;
  addNode?: Record<string, unknown>[] | undefined;
  rewire?: string[] | undefined;
  set?: string[] | undefined;
  dryRun?: boolean | undefined;
  out?: string | undefined;
  baseDir?: string | undefined;
}

export function patchArgv(input: PatchInput): string[] {
  const removeNode = input.removeNode ?? [];
  const addNode = input.addNode ?? [];
  const rewire = input.rewire ?? [];
  const set = input.set ?? [];
  if (removeNode.length + addNode.length + rewire.length + set.length === 0) {
    throw new Error("至少给一个动作：removeNode / addNode / rewire / set");
  }
  for (const r of rewire) {
    if (!/^[^:=]+:[^:=]+=[^:=]+:[^:=]+$/.test(r)) {
      throw new Error(`rewire 的写法是 <节点>:<端口>=<节点>:<端口>，收到 ${r}`);
    }
  }
  // dryRun 默认 true：一个能原地覆写图的工具，默认必须是「先给我看差异」
  const dryRun = input.dryRun ?? true;
  if (input.out && dryRun) {
    throw new Error("out 要配 dryRun:false —— dryRun 下什么都不写，给了 out 也一样");
  }
  const argv = ["patch", input.graphPath];
  if (input.baseDir) argv.push("--base-dir", input.baseDir);
  for (const id of removeNode) argv.push("--remove-node", id);
  for (const node of addNode) argv.push("--add-node", JSON.stringify(node));
  for (const r of rewire) argv.push("--rewire", r);
  for (const s of set) argv.push("--set", s);
  if (dryRun) argv.push("--dry-run");
  if (input.out) argv.push("-o", input.out);
  argv.push("--json");
  return argv;
}

export function perturbArgv(input: PerturbInput): string[] {
  if (!input.metric || input.metric.length === 0) {
    throw new Error("至少给一个 metric，例如 outputs.gap");
  }
  if (!input.after.includes(":")) {
    throw new Error(`after 的写法是 <nodeId>:<port>，收到 ${input.after}`);
  }
  const argv = ["perturb", input.graphPath];
  if (input.baseDir) argv.push("--base-dir", input.baseDir);
  argv.push("--after", input.after);
  argv.push("--region", JSON.stringify(input.region));
  argv.push("--axis", input.axis);
  argv.push(...samplesArgv(input));
  for (const m of input.metric) argv.push("--metric", m);
  if (input.expect !== undefined) argv.push("--expect", String(input.expect));
  if (input.tolerance !== undefined) argv.push("--tolerance", String(input.tolerance));
  if (input.csv) argv.push("--csv", input.csv);
  for (const s of input.set ?? []) argv.push("--set", s);
  if (input.noCache) argv.push("--no-cache");
  return argv;
}
