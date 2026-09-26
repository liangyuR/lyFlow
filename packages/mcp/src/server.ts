import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import type { Config } from "./config.js";
import { LyFlowHttp } from "./http.js";
import { registerResources } from "./resources.js";
import { registerTools } from "./tools.js";

export const INSTRUCTIONS =
  "LyFlow 的算子图平台。先 list_operators / get_operator 读清算子与参数说明，" +
  "再 validate_graph、run_graph；点云只看 summarize_output 的统计量。" +
  "批量调参走 eval，判断「测的是不是那条缝」走 perturb —— 顺序是 perturb 在 eval 之前。";

export function createServer(config: Config): McpServer {
  const http = new LyFlowHttp(config.httpBase, config.token);
  const server = new McpServer(
    { name: "lyflow", version: "1.1.0" },
    { instructions: INSTRUCTIONS },
  );
  registerTools(server, config, http);
  registerResources(server, http);
  return server;
}
