"""把 lyflow.overlay2d 画到图上（只是呈现，与编辑器 packages/editor/src/lib/overlay2d.ts 的 ROLE_STYLES 同一张配色表）。

坐标是图像像素，像素中心在整数坐标；role 先按整个查、再按第一段查（edge.left → edge），都没有就用默认的青色。
"""

from __future__ import annotations

import os

import cv2
import numpy as np

try:  # 中文要 PIL + 一款中文字体；没有就退回 OpenCV（中文会画成问号）
    from PIL import Image, ImageDraw, ImageFont

    _FONT_PATH = next((p for p in (os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", f)
                                   for f in ("msyh.ttc", "simhei.ttf", "simsun.ttc")) if os.path.exists(p)), None)
except ImportError:  # pragma: no cover
    Image = None
    _FONT_PATH = None

# 与编辑器同一张表（#rrggbb, 线宽）；OpenCV 是 BGR
ROLE_STYLES = {
    "nozzle": ("#fbbf24", 2), "target": ("#f472b6", 2), "link": ("#9ca3af", 1), "path": ("#4a9eff", 2),
    "sector": ("#a78bfa", 1), "zone": ("#a78bfa", 1), "coarse": ("#9ca3af", 1), "edge": ("#34d399", 1),
    "station": ("#34d399", 1), "bead": ("#34d399", 1), "ok": ("#34d399", 2), "missing": ("#f87171", 1),
    "break": ("#f87171", 3), "defect": ("#f87171", 3), "ng": ("#f87171", 2), "part": ("#f472b6", 2),
    "distance": ("#60a5fa", 1), "roi": ("#fbbf24", 1), "verdict": ("#e6e9ef", 1), "text": ("#e6e9ef", 1),
    "outlier": ("#9ca3af", 1),
}
DEFAULT = ("#22d3ee", 1)


def _bgr(hex_color: str):
    h = hex_color.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return (b, g, r)


def style(role: str):
    s = ROLE_STYLES.get(role) or ROLE_STYLES.get(role.replace(":", ".").replace("/", ".").split(".")[0]) or DEFAULT
    return _bgr(s[0]), s[1]


def _pt(p, k):
    return (int(round((p[0] + 0.5) * k - 0.5)), int(round((p[1] + 0.5) * k - 0.5)))


def draw(img_bgr: np.ndarray, items: list[dict], scale: float = 1.0, font: float = 0.5) -> np.ndarray:
    """items 画到 img 上（img 已经按 scale 缩放过；坐标按原图像素给）。"""
    out = img_bgr.copy()
    k = scale
    texts = []
    for it in items:
        color, width = style(it.get("role", "default"))
        kind = it.get("kind")
        pts = it.get("points") or []
        if kind == "polyline" and len(pts) >= 2:
            arr = np.array([_pt(p, k) for p in pts], np.int32).reshape(-1, 1, 2)
            cv2.polylines(out, [arr], bool(it.get("closed")), color, width, cv2.LINE_AA)
        elif kind == "segments":
            for a, b in zip(pts[0::2], pts[1::2]):
                cv2.line(out, _pt(a, k), _pt(b, k), color, width, cv2.LINE_AA)
        elif kind == "points":
            for p in pts:
                cv2.circle(out, _pt(p, k), max(1, width), color, -1, cv2.LINE_AA)
        elif kind == "circle":
            cv2.circle(out, _pt(it["center"], k), max(1, int(round(it["radius"] * k))), color, width, cv2.LINE_AA)
        elif kind == "box":
            cv2.rectangle(out, _pt(it["min"], k), _pt(it["max"], k), color, width, cv2.LINE_AA)
        elif kind == "text":
            texts.append((_pt(it["at"], k), it.get("text", ""), color))
    return put_texts(out, texts, size=max(12, int(28 * font)))


def put_texts(img: np.ndarray, texts, size: int = 14) -> np.ndarray:
    """[(基线左端 (x, y), 文字, BGR)] 写到图上，带一圈黑边好读。"""
    if not texts:
        return img
    if Image is None or _FONT_PATH is None:
        out = img.copy()
        for at, text, color in texts:
            cv2.putText(out, text, at, cv2.FONT_HERSHEY_SIMPLEX, size / 28, (0, 0, 0), 3, cv2.LINE_AA)
            cv2.putText(out, text, at, cv2.FONT_HERSHEY_SIMPLEX, size / 28, color, 1, cv2.LINE_AA)
        return out
    pil = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    d = ImageDraw.Draw(pil)
    f = ImageFont.truetype(_FONT_PATH, size)
    for (x, y), text, (b, g, r) in texts:
        d.text((x, y - size), text, font=f, fill=(r, g, b), stroke_width=2, stroke_fill=(0, 0, 0))
    return cv2.cvtColor(np.array(pil), cv2.COLOR_RGB2BGR)


def read_gray_as_bgr(path) -> np.ndarray:
    data = np.fromfile(str(path), dtype=np.uint8)
    img = cv2.imdecode(data, cv2.IMREAD_GRAYSCALE)
    return cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)


def write_image(path, img) -> None:
    ext = str(path).rsplit(".", 1)[-1]
    ok, buf = cv2.imencode("." + ext, img, [cv2.IMWRITE_JPEG_QUALITY, 88])
    if not ok:
        raise RuntimeError(f"编码失败：{path}")
    buf.tofile(str(path))


def caption(img: np.ndarray, text: str, color=(255, 255, 255), size: int = 15) -> np.ndarray:
    """顶上一条黑底说明。"""
    out = img.copy()
    cv2.rectangle(out, (0, 0), (out.shape[1], size + 8), (0, 0, 0), -1)
    return put_texts(out, [((4, size + 3), text, color)], size=size)


def contact_sheet(tiles: list[np.ndarray], cols: int) -> np.ndarray:
    h, w = tiles[0].shape[:2]
    rows = (len(tiles) + cols - 1) // cols
    sheet = np.zeros((rows * h, cols * w, 3), np.uint8)
    for i, t in enumerate(tiles):
        r, c = divmod(i, cols)
        sheet[r * h:(r + 1) * h, c * w:(c + 1) * w] = t
    return sheet
