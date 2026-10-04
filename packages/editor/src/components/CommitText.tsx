// 失焦或回车才提交的一行文字框（一次编辑一条撤销），Esc 撤回。图名、节点标题、子图名用它：以前这几处
// onChange 直接写 store，每敲一个字就是一条撤销 —— 中文输入法拼音没上屏的那几下也算，Ctrl+Z 得一个字一个字地
// 退回去，退的路上还有拼音。

import { useEffect, useRef, useState, type InputHTMLAttributes } from "react";

type Passthrough = Omit<InputHTMLAttributes<HTMLInputElement>, "value" | "onChange" | "onBlur" | "onFocus" | "onKeyDown">;

export function CommitText({
  value,
  onCommit,
  ...rest
}: Passthrough & { value: string; onCommit: (text: string) => void }) {
  const [text, setText] = useState(value);
  const editing = useRef(false);
  /** Esc 按下了：接着的那次 blur 撤回、不提交（同 NumberInput —— onBlur 拿到的还是打进去的那个 text） */
  const cancelled = useRef(false);
  // 外面的值变了（撤销、换了节点）且没在编辑时才同步，否则会在打字中途把框里的字抢走
  useEffect(() => {
    if (!editing.current) setText(value);
  }, [value]);

  const commit = () => {
    editing.current = false;
    if (cancelled.current) {
      cancelled.current = false;
      setText(value);
      return;
    }
    if (text !== value) onCommit(text);
  };

  return (
    <input
      {...rest}
      value={text}
      onFocus={() => (editing.current = true)}
      onChange={(e) => {
        editing.current = true;
        setText(e.target.value);
      }}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === "Enter") e.currentTarget.blur();
        if (e.key === "Escape") {
          cancelled.current = true;
          e.currentTarget.blur();
        }
      }}
    />
  );
}
