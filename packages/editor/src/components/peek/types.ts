import type { PeekSource } from "../../lib/peekSource";
import type { PeekWindow } from "../../store/peek";

export interface PeekViewProps {
  win: PeekWindow;
  src: PeekSource;
}
