//
// 极简 CDP 客户端。
//
// 为什么不用 puppeteer/playwright：它们要下载自己的浏览器，而我们要驱动的是
// **Tauri 里那个 WebView2**，不是另开一个 Chrome。真正需要的能力只有三件：
// 找到 target、开 WebSocket、发 Runtime.evaluate —— 一百来行，不值得为它
// 引一棵几百兆的依赖树，也不值得让验收依赖一次网络下载。
//
// Node 22+ 自带 WebSocket 与 fetch，所以这里零依赖。
//

/** 轮询 CDP 的 /json/list，直到出现一个页面 target。 */
export async function waitForTarget(port, { timeoutMs = 90_000, match } = {}) {
  const deadline = Date.now() + timeoutMs;
  let lastError = "还没开始";
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${port}/json/list`);
      const targets = await res.json();
      const page = targets.find(
        (t) => t.type === "page" && t.webSocketDebuggerUrl && (!match || match(t)),
      );
      if (page) return page;
      lastError = `有 ${targets.length} 个 target，但没有匹配的页面`;
    } catch (e) {
      lastError = e.message;
    }
    await sleep(300);
  }
  throw new Error(
    `等不到 CDP target（端口 ${port}）：${lastError}\n` +
      `检查 tauri dev 是否带着 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS 启动。`,
  );
}

export function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

export class Cdp {
  #ws;
  #nextId = 1;
  #pending = new Map();
  #listeners = new Map();

  static async connect(wsUrl) {
    const cdp = new Cdp();
    await cdp.#open(wsUrl);
    return cdp;
  }

  #open(wsUrl) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl);
      this.#ws = ws;
      ws.addEventListener("open", () => resolve());
      ws.addEventListener("error", (e) => reject(new Error(`CDP 连接失败: ${e.message ?? e}`)));
      ws.addEventListener("message", (event) => {
        const msg = JSON.parse(event.data);
        if (msg.id !== undefined) {
          const slot = this.#pending.get(msg.id);
          if (!slot) return;
          this.#pending.delete(msg.id);
          if (msg.error) slot.reject(new Error(`${msg.error.message} (${msg.error.code})`));
          else slot.resolve(msg.result);
          return;
        }
        for (const fn of this.#listeners.get(msg.method) ?? []) fn(msg.params);
      });
    });
  }

  on(method, fn) {
    const list = this.#listeners.get(method) ?? [];
    list.push(fn);
    this.#listeners.set(method, list);
  }

  send(method, params = {}) {
    const id = this.#nextId++;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#ws.send(JSON.stringify({ id, method, params }));
    });
  }

  /**
   * 在页面里跑一段表达式，拿回 JSON 化的结果。
   *
   * `awaitPromise` 默认开着：验收里几乎每一步都要等一个 Promise，
   * 忘了开的话拿到的是 `{}`（一个 Promise 的 JSON 形态），
   * 断言会以一种极其费解的方式失败。
   */
  async eval(expression, { awaitPromise = true } = {}) {
    const result = await this.send("Runtime.evaluate", {
      expression: `(async () => { ${expression} })()`,
      awaitPromise,
      returnByValue: true,
    });
    if (result.exceptionDetails) {
      const d = result.exceptionDetails;
      throw new Error(
        `页面里抛异常: ${d.exception?.description ?? d.text}\n表达式片段: ${expression.slice(0, 200)}`,
      );
    }
    return result.result.value;
  }

  /** 等页面里某个条件成立。返回该条件最后一次的值。 */
  async waitFor(expression, { timeoutMs = 30_000, intervalMs = 100, what = expression } = {}) {
    const deadline = Date.now() + timeoutMs;
    let last;
    while (Date.now() < deadline) {
      last = await this.eval(`return (${expression});`);
      if (last) return last;
      await sleep(intervalMs);
    }
    throw new Error(`等待超时（${timeoutMs}ms）：${what}\n最后一次的值：${JSON.stringify(last)}`);
  }

  close() {
    try {
      this.#ws?.close();
    } catch {
      /* 关就完了 */
    }
  }
}
