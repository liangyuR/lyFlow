import { dialogs } from "./dialogs";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";

export function pngFileName(baseName: string): string {
  const stamp = new Date().toISOString().replace(/[-:]/g, "").replace(/\..*$/, "");
  const name = (baseName || "view").replace(/[^\w.-]+/g, "_");
  return `${name}-${stamp}.png`;
}

export async function exportCanvasPng(
  canvas: HTMLCanvasElement,
  baseName: string,
): Promise<void> {
  const url = canvas.toDataURL("image/png");
  const file = pngFileName(baseName);

  const pickPath = dialogs().pickPath;
  if (!pickPath) {
    // 宿主没有保存对话框，退回让浏览器自己下载
    const a = document.createElement("a");
    a.href = url;
    a.download = file;
    document.body.appendChild(a);
    a.click();
    a.remove();
    return;
  }
  try {
    const picked = await pickPath({
      mode: "save",
      defaultPath: file,
      filters: [{ name: "PNG", extensions: ["png"] }],
    });
    if (typeof picked !== "string") return;
    const base64 = url.slice(url.indexOf(",") + 1);
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
    await transport.writeFileBytes(picked, bytes);
    useUiStore.getState().showToast(`已导出 ${picked}`);
  } catch (e) {
    useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
  }
}
