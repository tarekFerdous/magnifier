"""Renders the README media in docs/ by applying McScope's exact lens math
(src/render/LensShader.h) to frames.

The real lens can't be screen-recorded: the overlay is excluded from capture
(WDA_EXCLUDEFROMCAPTURE) so it never magnifies itself. So demos are made by
recording the game with the lens OFF and applying the lens offline.

Usage (needs Python 3 with Pillow + numpy; video files also need opencv-python):
    python tools/make_demo.py                 # synthetic scene illustration
    python tools/make_demo.py clip.mkv --start 41 --end 47.2 --crop 1280x720 \\
        --radius 220 --strength 3 --phases "off:2.3,gradual:1.6,fixed:1.5,off:0.8"
    python tools/make_demo.py clip.gif        # also: ScreenToGif GIFs or a folder of PNG/JPG frames
"""
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
W, H = 640, 360
PW = 2200            # panorama width (camera pans across it)
HORIZON = 171        # targets stand just below it, exactly at crosshair height (H/2)
rng = np.random.default_rng(7)


def target_x(i):
    return 260 + 230 * i


def font(size, bold=False):
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    try:
        return ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", name), size)
    except OSError:
        return ImageFont.load_default()


# ---------------------------------------------------------------- scene
def make_panorama():
    px = np.zeros((H, PW, 3), np.float32)
    t = np.linspace(0, 1, HORIZON)[:, None]
    px[:HORIZON] = (np.array([16, 22, 56]) * (1 - t) + np.array([238, 136, 86]) * t)[:, None, :]
    g = np.linspace(0, 1, H - HORIZON)[:, None]
    px[HORIZON:] = (np.array([52, 44, 54]) * (1 - g) + np.array([20, 18, 24]) * g)[:, None, :]
    img = Image.fromarray(px.astype(np.uint8))
    d = ImageDraw.Draw(img)

    # stars
    for _ in range(260):
        x, y = rng.integers(0, PW), rng.integers(0, HORIZON - 90)
        v = int(rng.integers(150, 255))
        d.point((x, y), fill=(v, v, v))

    d.ellipse((1180 - 34, 128 - 34, 1180 + 34, 128 + 34), fill=(255, 200, 128))

    xs = np.arange(PW)
    ridge = HORIZON - 52 - 22 * np.sin(xs / 150) - 10 * np.sin(xs / 41 + 1.3)
    d.polygon([(int(x), float(ridge[x])) for x in xs] + [(PW, HORIZON), (0, HORIZON)],
              fill=(78, 52, 86))

    # skyline with tiny lit windows: the fine detail the lens reveals
    x = 0
    while x < PW:
        w, h = int(rng.integers(14, 40)), int(rng.integers(18, 78))
        top = HORIZON - h
        shade = int(rng.integers(0, 14))
        d.rectangle((x, top, x + w - 1, HORIZON), fill=(30 + shade, 28 + shade, 46 + shade))
        for wy in range(top + 3, HORIZON - 3, 5):
            for wx in range(x + 2, x + w - 3, 4):
                if rng.random() < 0.42:
                    warm = rng.random() < 0.8
                    d.rectangle((wx, wy, wx + 1, wy + 2),
                                fill=(255, 212, 140) if warm else (150, 210, 255))
        x += w + int(rng.integers(0, 6))

    # ground perspective grid
    for k in range(1, 40):
        y = HORIZON + 2.2 * k ** 1.55
        if y >= H:
            break
        d.line((0, y, PW, y), fill=(74, 62, 88))
    for x0 in range(0, PW, 44):
        d.line((x0, HORIZON, x0 + (x0 - PW / 2) * 1.6, H), fill=(74, 62, 88))

    # distant targets with tiny labels
    small = font(9, bold=True)
    for i in range(9):
        tx, ty = target_x(i), H // 2
        d.line((tx, ty, tx, ty + 9), fill=(170, 160, 150))
        for rad, col in ((7, (230, 60, 60)), (5, (245, 245, 245)), (3, (230, 60, 60)), (1, (245, 245, 245))):
            d.ellipse((tx - rad, ty - rad, tx + rad, ty + rad), fill=col)
        d.text((tx + 10, ty - 6), f"T{i + 1}", font=small, fill=(255, 236, 200))
    return np.asarray(img).astype(np.float32)


def draw_crosshair(frame):
    """The game's own crosshair: part of the captured image, so the lens zooms it too."""
    img = Image.fromarray(frame.astype(np.uint8))
    d = ImageDraw.Draw(img)
    cx, cy = W // 2, H // 2
    for (x0, y0, x1, y1) in ((cx - 6, cy, cx - 2, cy), (cx + 2, cy, cx + 6, cy),
                             (cx, cy - 6, cx, cy - 2), (cx, cy + 2, cx, cy + 6)):
        d.line((x0, y0, x1, y1), fill=(0, 0, 0), width=3)
        d.line((x0, y0, x1, y1), fill=(120, 255, 140), width=1)
    return np.asarray(img).astype(np.float32)


# ---------------------------------------------------------------- lens (mirrors LensShader.h)
_grids = {}


def apply_lens(frame, radius, strength, gradual=True):
    if radius <= 0 or strength <= 1:
        return frame
    h, w = frame.shape[:2]
    if (h, w) not in _grids:
        _grids[(h, w)] = np.mgrid[0:h, 0:w].astype(np.float32) + 0.5   # pixel centers
    YS, XS = _grids[(h, w)]
    cx, cy = float(w // 2), float(h // 2)
    dx, dy = XS - cx, YS - cy
    r = np.hypot(dx, dy)
    t = np.clip(1 - r / radius, 0, 1)
    z = 1 + (strength - 1) * t * t if gradual else np.full_like(r, strength)
    sx, sy = cx + dx / z - 0.5, cy + dy / z - 0.5          # to texel space
    x0, y0 = np.floor(sx).astype(int), np.floor(sy).astype(int)
    wx, wy = (sx - x0)[..., None], (sy - y0)[..., None]
    x0c, x1c = np.clip(x0, 0, w - 1), np.clip(x0 + 1, 0, w - 1)
    y0c, y1c = np.clip(y0, 0, h - 1), np.clip(y0 + 1, 0, h - 1)
    sampled = (frame[y0c, x0c] * (1 - wx) * (1 - wy) + frame[y0c, x1c] * wx * (1 - wy) +
               frame[y1c, x0c] * (1 - wx) * wy + frame[y1c, x1c] * wx * wy)
    a = np.clip(radius - r, 0, 1)[..., None]               # 1px anti-aliased edge
    return frame * (1 - a) + sampled * a


# ---------------------------------------------------------------- overlays
def pill(d, xy, text, f, fg=(255, 255, 255), bg=(10, 10, 14, 190), anchor="ls"):
    x, y = xy
    l, t, r, b = d.textbbox((x, y), text, font=f, anchor=anchor)
    d.rounded_rectangle((l - 10, t - 7, r + 10, b + 7), radius=10, fill=bg)
    d.text((x, y), text, font=f, fill=fg, anchor=anchor)


def annotate(frame, caption, sub=None, tag="illustration · McScope lens math"):
    img = Image.fromarray(np.clip(frame, 0, 255).astype(np.uint8)).convert("RGBA")
    w, h = img.size
    k = h / 360  # scale overlay text with resolution
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    pill(d, (int(22 * k), h - int(22 * k)), caption, font(int(17 * k), bold=True))
    if sub:
        pill(d, (int(22 * k), h - int(62 * k)), sub, font(int(13 * k)), fg=(225, 225, 230))
    d.text((w - int(12 * k), int(12 * k)), tag, font=font(int(11 * k)),
           fill=(255, 255, 255, 170), anchor="ra")
    return Image.alpha_composite(img, layer).convert("RGB")


def view(pano, offset):
    o = int(offset)
    return draw_crosshair(pano[:, o:o + W].copy())


def save_gif(frames, name, ms, dither=True, colors=160):
    # One shared palette for all frames avoids flicker and keeps the file small.
    fw, fh = frames[0].size
    sample = Image.new("RGB", (fw, fh * 4))
    for i, f in enumerate(frames[:: max(1, len(frames) // 4)][:4]):
        sample.paste(f, (0, fh * i))
    pal = sample.quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    # Dithering helps smooth synthetic gradients but bloats photographic footage several-fold.
    mode = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    q = [f.quantize(palette=pal, dither=mode) for f in frames]
    path = os.path.join(OUT, name)
    q[0].save(path, save_all=True, append_images=q[1:], duration=ms, loop=0, optimize=True)
    print(f"{name}: {len(frames)} frames, {os.path.getsize(path) / 1024:.0f} KB")


def comparison_png(base, R, S, cw, ch, scale_to=None):
    """Off | Gradual | Fixed, cropped around the lens center of one frame.
    scale_to: resize each crop to this panel width (keeps big native crops readable)."""
    h, w = base.shape[:2]
    panels = [("Off", base), ("Gradual zoom", apply_lens(base, R, S, True)),
              ("Fixed zoom", apply_lens(base, R, S, False))]
    pw = scale_to or cw
    ph = round(ch * pw / cw)
    gap, pad, label_h = 12, 12, 34
    out = Image.new("RGB", (pad * 2 + pw * 3 + gap * 2, pad * 2 + ph + label_h), (13, 13, 16))
    d = ImageDraw.Draw(out)
    for i, (name, arr) in enumerate(panels):
        crop = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).crop(
            (w // 2 - cw // 2, h // 2 - ch // 2, w // 2 + cw // 2, h // 2 + ch // 2))
        if scale_to:
            crop = crop.resize((pw, ph), Image.LANCZOS)
        x = pad + i * (pw + gap)
        out.paste(crop, (x, pad))
        d.text((x + pw // 2, pad + ph + label_h // 2 + 2), name, font=font(16, bold=True),
               fill=(235, 235, 240), anchor="mm")
    path = os.path.join(OUT, "modes.png")
    out.save(path, optimize=True)
    print(f"modes.png: {os.path.getsize(path) / 1024:.0f} KB")


# ---------------------------------------------------------------- synthetic assets
def demo_gif(pano):
    """Camera flicks from target to target and rests on each, like aiming."""
    R, S = 120, 3.0
    stops = [target_x(i) - W // 2 for i in (1, 2, 3, 5, 6)]
    phases = ["off", "gradual", "gradual", "fixed", "off"]
    path = []
    for k, stop in enumerate(stops):
        if k:
            prev = stops[k - 1]
            for j in range(9):
                t = j / 9
                path.append((prev + (stop - prev) * t * t * (3 - 2 * t), k))
        path += [(stop, k)] * 14

    frames = []
    for offset, k in path:
        frame = view(pano, offset)
        mode = phases[k]
        if mode == "off":
            caption = "Lens off" if k == 0 else "Released  →  lens off"
            frames.append(annotate(frame, caption))
        elif mode == "gradual":
            frames.append(annotate(apply_lens(frame, R, S, True), "Hold Right Mouse  →  lens on",
                                   "Gradual zoom · 3.0x center · 120 px radius"))
        else:
            frames.append(annotate(apply_lens(frame, R, S, False), "Gradual zoom off  →  fixed 3.0x",
                                   "Same zoom across the whole lens (scope look)"))
    save_gif(frames, "demo.gif", 70)


def settings_gif(pano):
    base = view(pano, target_x(3) - W // 2)  # crosshair parked on a target
    frames = []

    def put(radius, strength):
        label = f"Radius {radius:.0f} px   ·   Strength {strength:.1f}x"
        frames.append(annotate(apply_lens(base, radius, strength, True), label))

    for i in range(32):
        put(150 * (i / 31) ** 0.8, 3.0)
    for _ in range(8):
        put(150, 3.0)
    for i in range(32):
        put(150, 3.0 + 3.0 * np.sin(i / 31 * np.pi / 2))
    for _ in range(8):
        put(150, 6.0)
    for i in range(24):
        put(150, 6.0 - 3.0 * (i / 23))
    for _ in range(10):
        put(150, 3.0)
    save_gif(frames, "settings.gif", 60)


def profile_svg():
    """Zoom vs. distance from center for both modes, S = 3. Light + dark via media query."""
    S = 3.0
    x0, y0, pw, ph = 64, 24, 520, 220          # plot area
    def X(u): return x0 + u * pw
    def Y(z): return y0 + ph - (z - 1) / (S + 0.2 - 1) * ph * 0.95 - ph * 0.02
    gradual = " ".join(f"{X(u):.1f},{Y(1 + (S - 1) * (1 - u) ** 2):.1f}"
                       for u in np.linspace(0, 1, 101))
    fixed = f"{X(0):.1f},{Y(S):.1f} {X(1):.1f},{Y(S):.1f} {X(1):.1f},{Y(1):.1f} {X(1.08):.1f},{Y(1):.1f}"
    ticks_y = "".join(
        f'<line class="grid" x1="{x0}" x2="{x0 + pw}" y1="{Y(z):.1f}" y2="{Y(z):.1f}"/>'
        f'<text class="tick" x="{x0 - 10}" y="{Y(z) + 4:.1f}" text-anchor="end">{z:g}x</text>'
        for z in (1, 2, 3))
    ticks_x = "".join(
        f'<text class="tick" x="{X(u):.1f}" y="{y0 + ph + 20}" text-anchor="middle">{lbl}</text>'
        for u, lbl in ((0, "center"), (0.5, "½ radius"), (1, "edge")))
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 700 300" width="700" height="300" role="img" aria-labelledby="t d">
<title id="t">Zoom vs. distance from the lens center</title>
<desc id="d">Strength 3x. Gradual: 3x at the center, 1.5x at half radius, easing to 1x at the edge. Fixed: 3x everywhere inside the lens, dropping to 1x at the edge.</desc>
<style>
  svg {{ --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781; --grid:#e1e0d9; --axis:#c3c2b7; --s1:#2a78d6; --s2:#eb6834; }}
  @media (prefers-color-scheme: dark) {{
    svg {{ --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7; --muted:#898781; --grid:#2c2c2a; --axis:#383835; --s1:#3987e5; --s2:#d95926; }}
  }}
  .bg {{ fill: var(--surface); }}
  .grid {{ stroke: var(--grid); stroke-width: 1; }}
  .axis {{ stroke: var(--axis); stroke-width: 1; }}
  .tick {{ fill: var(--muted); font: 12px system-ui, -apple-system, "Segoe UI", sans-serif; }}
  .label {{ fill: var(--ink2); font: 13px system-ui, -apple-system, "Segoe UI", sans-serif; }}
  .s1 {{ fill: none; stroke: var(--s1); stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }}
  .s2 {{ fill: none; stroke: var(--s2); stroke-width: 2; stroke-linejoin: round; stroke-dasharray: 6 4; }}
  .key1 {{ fill: var(--s1); }} .key2 {{ fill: var(--s2); }}
</style>
<rect class="bg" width="700" height="300" rx="8"/>
{ticks_y}
<line class="axis" x1="{x0}" x2="{x0 + pw}" y1="{Y(1) + 0.5:.1f}" y2="{Y(1) + 0.5:.1f}"/>
{ticks_x}
<polyline class="s2" points="{fixed}"/>
<polyline class="s1" points="{gradual}"/>
<text class="label" x="{X(0.30):.1f}" y="{Y(1 + 2 * 0.49) - 10:.1f}">Gradual</text>
<text class="label" x="{X(0.42):.1f}" y="{Y(S) - 8:.1f}">Fixed</text>
<g transform="translate({x0 + pw + 22}, {y0 + 8})">
  <rect class="key1" width="14" height="3" y="5" rx="1.5"/><text class="label" x="22" y="11">Gradual</text>
  <rect class="key2" width="14" height="3" y="27" rx="1.5"/><text class="label" x="22" y="33">Fixed</text>
</g>
</svg>
'''
    path = os.path.join(OUT, "zoom-profile.svg")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(svg)
    print("zoom-profile.svg written")


# ---------------------------------------------------------------- real footage (e.g. CS2)
FOOTAGE_TAG = "real gameplay · lens applied offline with McScope's exact math"


def center_crop(img, crop):
    """Crop around the screen center, which is where the lens sits. crop = (w, h) or None."""
    if not crop:
        return img
    h, w = img.shape[:2]
    cw, ch = min(crop[0], w), min(crop[1], h)
    x0, y0 = w // 2 - cw // 2, h // 2 - ch // 2
    return img[y0:y0 + ch, x0:x0 + cw]


def load_footage(path, start, end, out_fps, crop):
    """Video (any format OpenCV reads, e.g. MKV/MP4 from OBS), a GIF, or a folder of PNG/JPGs.
    Recorded with the lens OFF; returns native-resolution float frames and the frame duration."""
    frames, ms = [], 1000 / out_fps
    lower = path.lower()
    if os.path.isdir(path):
        names = sorted(n for n in os.listdir(path) if n.lower().endswith((".png", ".jpg", ".jpeg")))
        frames = [np.asarray(Image.open(os.path.join(path, n)).convert("RGB")) for n in names]
    elif lower.endswith(".gif"):
        gif = Image.open(path)
        try:
            while True:
                frames.append(np.asarray(gif.convert("RGB")))
                gif.seek(gif.tell() + 1)
        except EOFError:
            pass
    else:
        import cv2  # only needed for video files: pip install opencv-python
        cap = cv2.VideoCapture(path)
        if not cap.isOpened():
            raise SystemExit(f"cannot open {path}")
        src_fps = cap.get(cv2.CAP_PROP_FPS) or 30
        step = max(1, round(src_fps / out_fps))
        ms = 1000 * step / src_fps
        first = int(start * src_fps)
        last = int(end * src_fps) if end else int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        cap.set(cv2.CAP_PROP_POS_FRAMES, first)
        for i in range(first, last):
            ok, bgr = cap.read()
            if not ok:
                break
            if (i - first) % step == 0:
                frames.append(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
    if not frames:
        raise SystemExit(f"no frames found in {path}")
    return [center_crop(f, crop).astype(np.float32) for f in frames], ms


def footage_assets(args):
    crop = tuple(map(int, args.crop.split("x"))) if args.crop else None
    frames, ms = load_footage(args.source, args.start, args.end, args.fps, crop)
    R, S = args.radius, args.strength   # in source pixels, i.e. exactly the app's settings
    n = len(frames)
    total_s = n * ms / 1000

    # Phases: "off:2.3,gradual:1.6,fixed:1.5,off:0.8" in seconds; default splits 20/40/25/15 %.
    if args.phases:
        spec = [(p.split(":")[0], float(p.split(":")[1])) for p in args.phases.split(",")]
    else:
        spec = [("off", .2 * total_s), ("gradual", .4 * total_s), ("fixed", .25 * total_s), ("off", .15 * total_s)]
    bounds, acc = [], 0.0
    for mode, secs in spec:
        acc += secs
        bounds.append((acc, mode))

    out_w = args.width
    out = []
    for i, f in enumerate(frames):
        t = i * ms / 1000
        k = next((j for j, (b, _) in enumerate(bounds) if t < b), len(bounds) - 1)
        mode = bounds[k][1]
        if mode == "off":
            caption = f"{args.game} · lens off" if k == 0 else "Released  →  lens off"
            img = f
            sub = None
        elif mode == "gradual":
            img, caption = apply_lens(f, R, S, True), f"Hold {args.bind}  →  lens on"
            sub = f"Gradual zoom · {S:.1f}x center · {R} px radius"
        else:
            img, caption = apply_lens(f, R, S, False), "Gradual zoom off  →  fixed zoom"
            sub = f"{S:.1f}x across the whole lens"
        # Lens at native resolution first, then scale: what a viewer of that monitor sees.
        pil = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
        pil = pil.resize((out_w, round(pil.height * out_w / pil.width)), Image.LANCZOS)
        out.append(annotate(np.asarray(pil).astype(np.float32), caption, sub, tag=FOOTAGE_TAG))
    save_gif(out, "demo.gif", round(ms), dither=False, colors=128)

    # Off | Gradual | Fixed from the frame at --compare-at (seconds into the clip).
    base = frames[min(n - 1, int(args.compare_at * 1000 / ms))] if args.compare_at is not None else frames[n // 2]
    side = 2 * R + 40
    comparison_png(base, R, S, side, min(side, base.shape[0]), scale_to=300)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", help="video / GIF / frame folder recorded with the lens OFF")
    ap.add_argument("--start", type=float, default=0, help="clip start, seconds (video only)")
    ap.add_argument("--end", type=float, default=None, help="clip end, seconds (video only)")
    ap.add_argument("--fps", type=float, default=15, help="GIF frame rate")
    ap.add_argument("--crop", default=None, help="center crop at source resolution, e.g. 1280x720")
    ap.add_argument("--width", type=int, default=800, help="GIF width")
    ap.add_argument("--radius", type=int, default=200, help="lens radius in source pixels")
    ap.add_argument("--strength", type=float, default=2.5)
    ap.add_argument("--phases", default=None, help='e.g. "off:2.3,gradual:1.6,fixed:1.5,off:0.8"')
    ap.add_argument("--compare-at", type=float, default=None, help="seconds into the clip for modes.png")
    ap.add_argument("--game", default="CS2")
    ap.add_argument("--bind", default="Right Mouse")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    pano = make_panorama()
    if args.source:
        footage_assets(args)
    else:
        demo_gif(pano)
        comparison_png(view(pano, target_x(3) - W // 2), 120, 3.0, 300, 270)
    settings_gif(pano)
    profile_svg()
