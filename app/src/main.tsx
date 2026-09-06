import React from "react";
import ReactDOM from "react-dom/client";

import { LyFlowEditor, TauriTransport, StaticTransport, type Transport } from "@lyflow/editor";

import { installDevBridge } from "./devbridge";
import { tauriDialogs } from "./dialogs";
import { installWindowTitle } from "./title";

import "./shell.css";

const root = document.getElementById("root");
if (!root) throw new Error("找不到 #root");

function inTauri(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

const transport: Transport = inTauri() ? new TauriTransport() : new StaticTransport();

// 验收脚本（scripts/e2e）用的窗口桥。装在渲染之前，
// 免得脚本连上来时 store 的订阅还没建立、错过最早的几次状态变迁。
installDevBridge(transport);
installWindowTitle();

ReactDOM.createRoot(root).render(
  <React.StrictMode>
    <LyFlowEditor transport={transport} dialogs={tauriDialogs} />
  </React.StrictMode>,
);
