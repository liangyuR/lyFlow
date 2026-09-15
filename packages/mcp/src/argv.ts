export interface SampleSelector {
  samplesPath?: string | undefined;
  samplesGlob?: string | undefined;
  bind?: string | undefined;
}

export interface EvalInput extends SampleSelector {
  graphPath: string;
  params?: Record<string, unknown>[] | undefined;
  param?: string[] | undefined;
  metric: string[];
  holdout?: string | undefined;
  groupBy?: string | undefined;
  baseDir?: string | undefined;
  set?: string[] | undefined;
  noCache?: boolean | undefined;
}

export interface PerturbInput extends SampleSelector {
  graphPath: string;
  after: string;
  region: unknown;
  axis: string;
  metric: string[];
  expect?: number | undefined;
  tolerance?: number | undefined;
  baseDir?: string | undefined;
  set?: string[] | undefined;
}

function samplesArgv(input: SampleSelector): string[] {
  if (input.samplesPath && input.samplesGlob) {
    throw new Error("samplesPath 与 samplesGlob 只能给一个");
  }
  if (input.samplesPath) {
    if (input.bind) throw new Error("bind 只跟 samplesGlob 配套");
    return ["--samples", input.samplesPath];
  }
  if (input.samplesGlob) {
    if (!input.bind) throw new Error("samplesGlob 要配一个 bind（<节点>.<参数>）");
    return ["--samples-glob", input.samplesGlob, "--bind", input.bind];
  }
  if (input.bind) throw new Error("bind 要和 samplesGlob 一起给");
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
  for (const s of input.set ?? []) argv.push("--set", s);
  if (input.noCache) argv.push("--no-cache");
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
  for (const s of input.set ?? []) argv.push("--set", s);
  return argv;
}
