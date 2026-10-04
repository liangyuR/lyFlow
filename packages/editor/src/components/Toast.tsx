// 连线被拒绝的原因、保存成功之类的短提示（ui store 的 showToast）。

import { useEffect, useState } from "react";

import { useUiStore } from "../store/ui";

/** 提示停多久：短的 2.6 秒；长句按字数加（一秒读五六个字），警告至少 4 秒，最长 8 秒。
 *  以前一律 2.6 秒：「这个输入端口已有连线（输入是单连接）：拖它的线头才是改接」这样的句子没读完就没了。 */
function toastMs(text: string, kind: "info" | "warn"): number {
  return Math.min(8000, Math.max(kind === "warn" ? 4000 : 2600, 1200 + [...text].length * 170));
}

/** 鼠标停在上面时不消失，移开后重新计时。 */
export function Toast() {
  const toast = useUiStore((s) => s.toast);
  const hideToast = useUiStore((s) => s.hideToast);
  const [hovered, setHovered] = useState(false);

  useEffect(() => {
    if (!toast) {
      setHovered(false);
      return;
    }
    if (hovered) return;
    const t = setTimeout(hideToast, toastMs(toast.text, toast.kind));
    return () => clearTimeout(t);
  }, [toast, hovered, hideToast]);

  if (!toast) return null;
  return (
    <div
      className={`toast toast--${toast.kind}`}
      data-testid="toast"
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
    >
      {toast.text}
    </div>
  );
}
