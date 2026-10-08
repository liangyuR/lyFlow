import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import type { Config } from "./config.js";
import { LyFlowHttp } from "./http.js";
import { registerResources } from "./resources.js";
import { registerTools } from "./tools.js";

export const INSTRUCTIONS =
  "LyFlow 算子图平台。先 get_environment 核对 HTTP/CLI 身份，inspect_graph 查看生效参数与绑定，" +
  "validate_graph、plan_graph、run_graph 使用同一组 graphParams/recipe/set。绑定参数只改 graphParams。" +
  "执行状态、定位/量测有效性、产品 OK/NG 分别查看。复杂值用 summarize_output 分页与 artifact，" +
  "图像用 view_output_image 配 overlay 与原图 ROI。glue 仅运行 full，不用预览图量测。" +
  "带真值的 eval 固定工件组与 train/validation/holdout；start_tuning 只用 train，锁住 acceptance，" +
  "validation 比较候选，holdout 最后验收，再 export_candidate。perturb 专用于点云几何位移；" +
  "glue 用已标注断口、定位和量测真值。长任务 get_job/cancel_job/resume_job，预算不足时显式追加。";

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
