// 文件对话框由宿主注入：Tauri 壳给原生对话框，浏览器宿主给自己的实现。
// 编辑器包本身不认识 @tauri-apps/plugin-dialog。

export class NoDialogError extends Error {
  constructor() {
    super("当前宿主没有提供文件对话框");
  }
}

export interface PathPickRequest {
  mode: "open" | "save" | "dir";
  filters?: { name: string; extensions: string[] }[];
  defaultPath?: string;
}

export interface EditorDialogs {
  /** 参数表单的路径选择与 3D 导出用。没有它的宿主会退回浏览器下载/提示。 */
  pickPath?: ((request: PathPickRequest) => Promise<string | null>) | undefined;
  /** 返回 null 表示用户取消。 */
  pickOpenPath(): Promise<string | null>;
  pickSavePath(suggested: string): Promise<string | null>;
  /** 有未保存改动时问一句。返回 true 表示可以继续。 */
  confirmDiscard(dirty: boolean): Promise<boolean>;
  /** 备份比正文新时问一句。返回 true 表示用户要恢复。 */
  confirmRestore(path: string, message: string): Promise<boolean>;
}

export const browserDialogs: EditorDialogs = {
  async pickOpenPath() {
    throw new NoDialogError();
  },
  async pickSavePath() {
    throw new NoDialogError();
  },
  async confirmDiscard(dirty) {
    if (!dirty) return true;
    return window.confirm("当前图有未保存的改动，确定放弃吗？");
  },
  async confirmRestore(_path, message) {
    return window.confirm(message);
  },
};

let current: EditorDialogs = browserDialogs;

export function setDialogs(next: EditorDialogs | undefined): void {
  current = next ?? browserDialogs;
}

export function dialogs(): EditorDialogs {
  return current;
}
