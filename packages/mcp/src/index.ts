import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { loadConfig } from "./config.js";
import { createServer } from "./server.js";

async function main(): Promise<void> {
  let config;
  try {
    config = loadConfig();
  } catch (e) {
    process.stderr.write(`${e instanceof Error ? e.message : String(e)}\n`);
    process.exitCode = 1;
    return;
  }
  const server = createServer(config);
  await server.connect(new StdioServerTransport());
  process.stderr.write(`lyflow-mcp: ${config.httpBase}${config.cli ? ` + ${config.cli}` : ""}\n`);
}

void main();
