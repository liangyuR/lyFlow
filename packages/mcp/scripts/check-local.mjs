import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';

const launcher = path.join(path.dirname(fileURLToPath(import.meta.url)), 'local.mjs');
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [launcher, ...process.argv.slice(2)],
  env: { ...process.env },
  stderr: 'inherit',
});
const client = new Client({ name: 'lyflow-local-check', version: '1.0.0' });
try {
  await client.connect(transport);
  const tools = (await client.listTools()).tools.map((tool) => tool.name);
  for (const name of ['get_environment', 'inspect_graph', 'run_graph', 'summarize_output']) {
    if (!tools.includes(name)) throw new Error(`MCP 缺少工具：${name}`);
  }
  const environment = await client.callTool({ name: 'get_environment', arguments: {} });
  if (
    environment.isError ||
    environment.structuredContent?.compatible !== true ||
    environment.structuredContent?.cli?.available !== true
  ) {
    throw new Error(JSON.stringify(environment));
  }
  console.log(JSON.stringify({ tools, environment }, null, 2));
} finally {
  await client.close();
}
