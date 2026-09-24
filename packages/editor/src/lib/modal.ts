// 编辑器自己的小对话框（param-recipe P3）：起配方名、确认删除、「文件已被外部修改」的三选一。
// 不走宿主的 EditorDialogs：那一套只有文件选择与是 / 否两种，而这里要输入框、校验和多个选项；
// 画在编辑器里也让 e2e 能像点别的按钮一样点它们。Promise 形式，调用方 await 结果。

import { create } from "zustand";

export interface ModalChoice {
  id: string;
  label: string;
  tone?: "primary" | "danger" | "plain" | undefined;
}

export interface TextRequest {
  kind: "text";
  title: string;
  message?: string | undefined;
  value: string;
  placeholder?: string | undefined;
  okLabel?: string | undefined;
  /** 返回不能用的原因；null = 能用。每敲一个字都查，OK 按钮随之置灰。 */
  validate?: ((value: string) => string | null) | undefined;
  /** 可选的一个勾选项（新建配方时的「复制当前配方的值」）。 */
  checkbox?: { label: string; checked: boolean } | undefined;
}

export interface ChoiceRequest {
  kind: "choice";
  title: string;
  message?: string | undefined;
  /** 列在正文下面的条目（被外部修改的文件名之类）。 */
  items?: string[] | undefined;
  choices: ModalChoice[];
}

export type TextAnswer = { value: string; checked: boolean };

type Pending =
  | (TextRequest & { id: number; resolve: (v: TextAnswer | null) => void })
  | (ChoiceRequest & { id: number; resolve: (v: string | null) => void });

interface ModalState {
  current: Pending | null;
}

export const useModalStore = create<ModalState>(() => ({ current: null }));

let seq = 0;

/** 一次只开一个：新的来了，旧的按取消收掉。 */
function open(p: Pending): void {
  const prev = useModalStore.getState().current;
  if (prev) (prev.resolve as (v: null) => void)(null);
  useModalStore.setState({ current: p });
}

export function askText(req: Omit<TextRequest, "kind">): Promise<TextAnswer | null> {
  return new Promise((resolve) => {
    seq += 1;
    open({ ...req, kind: "text", id: seq, resolve });
  });
}

export function askChoice(req: Omit<ChoiceRequest, "kind">): Promise<string | null> {
  return new Promise((resolve) => {
    seq += 1;
    open({ ...req, kind: "choice", id: seq, resolve });
  });
}

/** 关掉当前的对话框并给出答案（组件用）。 */
export function settleModal(answer: TextAnswer | string | null): void {
  const cur = useModalStore.getState().current;
  if (!cur) return;
  useModalStore.setState({ current: null });
  (cur.resolve as (v: typeof answer) => void)(answer);
}
