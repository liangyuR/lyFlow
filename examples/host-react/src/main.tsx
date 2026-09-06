import React, { useEffect, useState } from "react";
import ReactDOM from "react-dom/client";

import { HttpTransport, LyFlowEditor, useUiStore } from "@lyflow/editor";

import { installHostBridge } from "./bridge";
import "./host.css";

const API = import.meta.env["VITE_LYFLOW_API"] ?? "http://127.0.0.1:8787";
const TOKEN = import.meta.env["VITE_LYFLOW_TOKEN"] ?? "";

const transport = new HttpTransport(API, TOKEN || undefined);

/** 宿主自己调一个包里创建的 store 的 hook。两份 React 的话这里会直接抛
 *  “Invalid hook call” —— 所以它就是 peerDependency 复用的运行期证据。 */
function ReactInstanceProbe() {
  const depth = useUiStore((s) => s.path.length);
  const [ok, setOk] = useState(false);
  useEffect(() => {
    window.__lyflowHost = { react: React.version, hookOk: true, depth };
    setOk(true);
  }, [depth]);
  return (
    <span data-testid="host-probe" data-hook-ok={ok ? "1" : "0"} data-react={React.version}>
      React {React.version} · 层级 {depth}
    </span>
  );
}

function Host() {
  const [dirty, setDirty] = useState(false);
  const [name, setName] = useState("未命名");

  return (
    <div className="host">
      <header className="host__bar">
        <strong>宿主示例</strong>
        <span data-testid="host-doc">
          {name}
          {dirty ? " *" : ""}
        </span>
        <span data-testid="host-api">{API}</span>
        <ReactInstanceProbe />
      </header>
      <div className="host__editor">
        <LyFlowEditor
          transport={transport}
          onDocChange={(doc, isDirty) => {
            setDirty(isDirty);
            setName(doc.name ?? "未命名");
          }}
        />
      </div>
    </div>
  );
}

declare global {
  interface Window {
    __lyflowHost?: { react: string; hookOk: boolean; depth: number };
  }
}

const root = document.getElementById("root");
if (!root) throw new Error("找不到 #root");

installHostBridge(transport);

ReactDOM.createRoot(root).render(
  <React.StrictMode>
    <Host />
  </React.StrictMode>,
);
