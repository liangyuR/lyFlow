import assert from "node:assert/strict";
import test from "node:test";

import { parseJsonLines, runCli, stderrTail } from "../src/cli.js";
import { loadConfig } from "../src/config.js";

test("CLI 输出解析：stderr 掺进 stdout 的行被跳过而不是让解析崩掉；stderrTail 取最后一条非空行", () => {
  const chunk = [
    '{"kind":"eval_row","sample":"a","status":"ok"}',
    "3 组参数 × 51 个样本 = 153 次运行",
    "",
    '{"kind":"eval_summary","metric":"outputs.gap"}',
    "{ 这不是 JSON",
    "[1,2,3]",
    '"只是一个字符串"',
  ].join("\r\n");
  const parsed = parseJsonLines(chunk);
  assert.equal(parsed.lines.length, 2);
  assert.equal(parsed.lines[0]?.value["kind"], "eval_row");
  assert.equal(parsed.lines[1]?.value["kind"], "eval_summary");
  assert.deepEqual(parsed.skipped, [
    "3 组参数 × 51 个样本 = 153 次运行",
    "{ 这不是 JSON",
    "[1,2,3]",
    '"只是一个字符串"',
  ]);

  assert.equal(stderrTail("第一行\n第二行\n\n"), "第二行");
  assert.equal(stderrTail(""), "");
});

test("CLI 路径不存在时把起进程的错原样带回来", async () => {
  const config = loadConfig({
    LYFLOW_HTTP_BASE: "http://127.0.0.1:1",
    LYFLOW_CLI: "D:/这个路径肯定不存在/lyflow.exe",
  });
  const result = await runCli(config, ["manifest"]);
  assert.equal(result.code, -1);
  assert.ok(result.spawnError !== null);
  assert.match(result.stderr, /失败/);
});
