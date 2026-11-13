import type { PythonRuntimeConfig, PythonSettings } from '../transport/types';
import { cleanPathText } from './params';

export function defaultPythonConfig(): PythonRuntimeConfig {
  return { executable: '', packagePaths: [], timeoutMs: 60_000 };
}

export function unsupportedPythonSettings(): PythonSettings {
  const config = defaultPythonConfig();
  return {
    supported: false,
    editable: false,
    config,
    status: { ...config, configured: false, ready: false, operatorCount: 0, problems: [] },
  };
}

export function normalizePythonConfig(config: PythonRuntimeConfig): PythonRuntimeConfig {
  return {
    executable: cleanPathText(config.executable),
    packagePaths: [...new Set(config.packagePaths.map(cleanPathText).filter(Boolean))],
    timeoutMs: config.timeoutMs,
  };
}

export function pythonConfigProblem(config: PythonRuntimeConfig): string | null {
  if (!config.executable.trim()) return '请先选择 Python 解释器。';
  if (
    !Number.isSafeInteger(config.timeoutMs) ||
    config.timeoutMs < 1 ||
    config.timeoutMs > 86_400_000
  )
    return '超时必须是 1 至 86,400,000 的整数毫秒（最多一天）。';
  return null;
}
