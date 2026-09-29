"""Render the original PLY and the SOG from identical cameras with splat-transform's renderer and
compare them (PSNR over all pixels and over non-background pixels), plus side-by-side images.

usage: .venv/bin/python render_compare.py <original.ply> <asset.sog> <out_dir>
Cameras are in splat-transform's render space, which negates the file's y axis (and x) so the
y-down 3DGS data renders upright; file-frame point (x, y, z) is (-x, -y, z) here.
"""
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CAMERAS = {
    "lawn_eye_level_a": ((1.626, 0.25, -0.454), (-1.0, 0.18, -0.454)),
    "lawn_eye_level_b": ((-0.6, 0.25, -0.9), (2.4, 0.15, 0.2)),
    "overview": ((-4.582, 3.656, -6.278), (0.018, -0.344, -0.078)),
}
RES = "1280x720"


def render(src, cam, target, out):
    fmt = lambda v: ",".join(f"{c:.4f}" for c in v)
    cmd = ["npx", "splat-transform", "-w", "-q", f"--camera-pos={fmt(cam)}", f"--camera-target={fmt(target)}",
           "--resolution", RES, src, out]
    subprocess.run(cmd, cwd=HERE, check=True)
    return np.asarray(Image.open(out).convert("RGB"), np.float64)


def psnr(a, b, mask=None):
    d = (a - b) ** 2
    mse = d[mask].mean() if mask is not None else d.mean()
    return float(10 * np.log10(255.0 ** 2 / mse)) if mse > 0 else float("inf")


def main(ply, sog, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    results = {}
    for name, (cam, target) in CAMERAS.items():
        a = render(ply, cam, target, os.path.join(out_dir, f"{name}_ply.webp"))
        b = render(sog, cam, target, os.path.join(out_dir, f"{name}_sog.webp"))
        fg = (a.max(2) > 8) | (b.max(2) > 8)
        diff = np.abs(a - b).max(2)
        results[name] = {
            "psnr_all_db": round(psnr(a, b), 2),
            "psnr_foreground_db": round(psnr(a, b, fg), 2),
            "foreground_fraction": round(float(fg.mean()), 3),
            "pixels_diff_gt_8_levels_pct": round(float((diff > 8).mean() * 100), 3),
            "pixels_diff_gt_16_levels_pct": round(float((diff > 16).mean() * 100), 3),
        }
        # side-by-side (original | SOG | difference x8) at half size for the docs
        h, w = a.shape[:2]
        heat = np.clip(diff * 8, 0, 255).astype(np.uint8)
        panel = Image.new("RGB", (w * 3 // 2, h // 2 + 28), (20, 20, 20))
        for k, im in enumerate([a.astype(np.uint8), b.astype(np.uint8), np.stack([heat] * 3, 2)]):
            panel.paste(Image.fromarray(im).resize((w // 2, h // 2)), (k * w // 2, 28))
        draw = ImageDraw.Draw(panel)
        for k, label in enumerate(["original PLY (607.7 MB)", "SOG (38.8 MB)", "|difference| x8"]):
            draw.text((k * w // 2 + 8, 8), label, fill=(235, 235, 235))
        panel.save(os.path.join(out_dir, f"{name}_compare.jpg"), quality=88)
        print(name, results[name])
    with open(os.path.join(out_dir, "render_compare.json"), "w") as f:
        json.dump(results, f, indent=2)


if __name__ == "__main__":
    main(*sys.argv[1:4])
