// lib/modal.ts 的那几个对话框画在这里。挂在编辑器根元素里（不 portal 到 body）：宿主页面的样式碰不到它，
// 主题变量照常生效。键盘：回车 = 确定（输入框合格时）、Esc = 取消；按键不往外冒，编辑器的快捷键不会跟着响。

import { useEffect, useRef, useState } from "react";

import { settleModal, useModalStore } from "../lib/modal";

import "../styles.recipe.css";

export function Modal() {
  const current = useModalStore((s) => s.current);
  if (!current) return null;
  return (
    <div className="lf-modal__backdrop" data-testid="modal-backdrop" onPointerDown={(e) => e.stopPropagation()}>
      {current.kind === "text" ? <TextModal key={current.id} /> : <ChoiceModal key={current.id} />}
    </div>
  );
}

function TextModal() {
  const req = useModalStore((s) => s.current);
  const [value, setValue] = useState(req?.kind === "text" ? req.value : "");
  const [checked, setChecked] = useState(req?.kind === "text" ? (req.checkbox?.checked ?? false) : false);
  const input = useRef<HTMLInputElement>(null);
  useEffect(() => {
    input.current?.focus();
    input.current?.select();
  }, []);
  if (!req || req.kind !== "text") return null;
  const problem = req.validate?.(value) ?? null;
  const ok = () => {
    if (problem === null) settleModal({ value, checked });
  };
  return (
    <div
      className="lf-modal"
      role="dialog"
      aria-label={req.title}
      data-testid="modal"
      data-kind="text"
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === "Escape") settleModal(null);
        if (e.key === "Enter") ok();
      }}
    >
      <h3 className="lf-modal__title">{req.title}</h3>
      {req.message && <p className="lf-modal__msg">{req.message}</p>}
      <input
        ref={input}
        className="ctl ctl--str lf-modal__input"
        data-testid="modal-input"
        value={value}
        placeholder={req.placeholder}
        spellCheck={false}
        onChange={(e) => setValue(e.target.value)}
      />
      <p className="lf-modal__problem" data-testid="modal-problem">
        {problem ?? " "}
      </p>
      {req.checkbox && (
        <label className="lf-modal__check">
          <input
            type="checkbox"
            data-testid="modal-check"
            checked={checked}
            onChange={(e) => setChecked(e.target.checked)}
          />
          {req.checkbox.label}
        </label>
      )}
      <div className="lf-modal__buttons">
        <button type="button" className="lf-btn" data-testid="modal-cancel" onClick={() => settleModal(null)}>
          取消
        </button>
        <button
          type="button"
          className="lf-btn lf-btn--primary"
          data-testid="modal-ok"
          disabled={problem !== null}
          onClick={ok}
        >
          {req.okLabel ?? "确定"}
        </button>
      </div>
    </div>
  );
}

function ChoiceModal() {
  const req = useModalStore((s) => s.current);
  const first = useRef<HTMLButtonElement>(null);
  useEffect(() => first.current?.focus(), []);
  if (!req || req.kind !== "choice") return null;
  return (
    <div
      className="lf-modal"
      role="dialog"
      aria-label={req.title}
      data-testid="modal"
      data-kind="choice"
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === "Escape") settleModal(null);
      }}
    >
      <h3 className="lf-modal__title">{req.title}</h3>
      {req.message && <p className="lf-modal__msg">{req.message}</p>}
      {req.items && req.items.length > 0 && (
        <ul className="lf-modal__items">
          {req.items.map((it) => (
            <li key={it}>{it}</li>
          ))}
        </ul>
      )}
      <div className="lf-modal__buttons">
        {req.choices.map((c, i) => (
          <button
            key={c.id}
            ref={i === 0 ? first : undefined}
            type="button"
            className={`lf-btn${c.tone === "primary" ? " lf-btn--primary" : c.tone === "danger" ? " lf-btn--danger" : ""}`}
            data-testid={`modal-choice-${c.id}`}
            onClick={() => settleModal(c.id)}
          >
            {c.label}
          </button>
        ))}
      </div>
    </div>
  );
}
