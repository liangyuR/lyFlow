import { mkdtemp, writeFile, readFile, rm, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { renderSvg, renderGridSvg, sceneMetadata, gridScenes } from './index.js';
const exec = promisify(execFile);
async function browser(explicit) {
  const candidates = [
    explicit,
    process.env.LYFLOW_BROWSER,
    process.env.ProgramFiles &&
      join(process.env.ProgramFiles, 'Google/Chrome/Application/chrome.exe'),
    process.env['ProgramFiles(x86)'] &&
      join(process.env['ProgramFiles(x86)'], 'Microsoft/Edge/Application/msedge.exe'),
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/usr/bin/google-chrome',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  ].filter(Boolean);
  for (const p of candidates) {
    try {
      await access(p);
      return p;
    } catch {}
  }
  throw new Error(
    'No headless browser found; set LYFLOW_BROWSER to Edge/Chrome/Chromium executable',
  );
}
async function rasterize(svg, options) {
  const exe = await browser(options.browserPath),
    dir = await mkdtemp(join(tmpdir(), 'lyflow-render-'));
  const width = Number(svg.match(/width="(\d+)"/)[1]),
    height = Number(svg.match(/height="(\d+)"/)[1]);
  try {
    const html = join(dir, 'scene.html'),
      output = join(dir, 'scene.png');
    await writeFile(
      html,
      `<!doctype html><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src data:; style-src 'unsafe-inline'"><style>html,body{margin:0;padding:0;background:white;overflow:hidden}svg{display:block}</style>${svg}`,
    );
    await exec(
      exe,
      [
        '--headless',
        '--disable-gpu',
        '--disable-extensions',
        '--no-first-run',
        '--no-default-browser-check',
        '--hide-scrollbars',
        `--user-data-dir=${join(dir, 'profile')}`,
        `--window-size=${width},${height}`,
        '--force-device-scale-factor=1',
        `--screenshot=${output}`,
        pathToFileURL(html).href,
      ],
      { windowsHide: true, timeout: options.timeoutMs ?? 30000, maxBuffer: 1024 * 1024 },
    );
    const png = await readFile(output);
    if (
      png.length < 24 ||
      png.toString('hex', 0, 8) !== '89504e470d0a1a0a' ||
      png.readUInt32BE(16) !== width ||
      png.readUInt32BE(20) !== height
    )
      throw new Error('Browser returned invalid PNG or wrong dimensions');
    return png;
  } finally {
    await rm(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
}
export async function renderPng(scene, options = {}) {
  const svg = renderSvg(scene);
  return { png: await rasterize(svg, options), svg, metadata: sceneMetadata(scene) };
}
export async function renderGridPng(scenes, options = {}) {
  const svg = renderGridSvg(scenes, options);
  return {
    png: await rasterize(svg, options),
    svg,
    metadata: gridScenes(scenes, options).map(sceneMetadata),
  };
}
