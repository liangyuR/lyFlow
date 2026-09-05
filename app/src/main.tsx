import React from "react";
import ReactDOM from "react-dom/client";

import App from "./App";
import { installDevBridge } from "./lib/devbridge";
import "./styles.css";
import "./styles.editor.css";

const root = document.getElementById("root");
if (!root) throw new Error("找不到 #root");

// 验收脚本（scripts/e2e）用的窗口桥。装在渲染之前，
// 免得脚本连上来时 store 的订阅还没建立、错过最早的几次状态变迁。
installDevBridge();

ReactDOM.createRoot(root).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
