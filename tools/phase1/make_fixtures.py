"""Generate small SOG fixtures (valid and malformed) plus the expected loader results.

usage: ../phase0/.venv/bin/python make_fixtures.py <out_dir>
Writes <out_dir>/<name>.sog (or a folder) and <out_dir>/fixtures.json:
  {name: {"path": ..., "error": "<sog::Error name>", "records": [[5 x uint32], ...] (valid only)}}
"""
import io
import json
import os
import shutil
import sys
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "phase0"))
import sog_decode as sd  # noqa: E402

N = 100                     # splats
W, H = 16, 7                # 112 texels >= N
rng = np.random.default_rng(1234)


def webp(arr, lossless=True):
    buf = io.BytesIO()
    if lossless:
        Image.fromarray(arr).save(buf, "WEBP", lossless=True, exact=True, quality=100, method=4)
    else:
        Image.fromarray(arr).save(buf, "WEBP", lossless=False, quality=80)
    return buf.getvalue()


def texels(fill):
    a = np.zeros((H * W, fill.shape[1]), np.uint8)
    a[:N] = fill
    return a.reshape(H, W, fill.shape[1])


def base_asset(bands=1, palette=8):
    means_q = rng.integers(0, 65536, (N, 3), dtype=np.uint32)
    means_l = (means_q & 0xFF).astype(np.uint8)
    means_u = (means_q >> 8).astype(np.uint8)
    quats = rng.integers(0, 256, (N, 4), dtype=np.uint16).astype(np.uint8)
    quats[:, 3] = 252 + rng.integers(0, 4, N)
    scales = rng.integers(0, 256, (N, 3), dtype=np.uint16).astype(np.uint8)
    sh0 = rng.integers(0, 256, (N, 4), dtype=np.uint16).astype(np.uint8)
    sh0[:5, 3] = 0                                        # fully transparent splats keep their colour
    files = {
        "means_l.webp": webp(texels(means_l)), "means_u.webp": webp(texels(means_u)),
        "quats.webp": webp(texels(quats)), "scales.webp": webp(texels(scales)), "sh0.webp": webp(texels(sh0)),
    }
    meta = {
        "version": 2, "asset": {"generator": "nanogs-sog fixtures"}, "count": N,
        "means": {"mins": [-2.0, -1.5, -3.25], "maxs": [2.5, 1.0, 3.0], "files": ["means_l.webp", "means_u.webp"]},
        "scales": {"codebook": np.linspace(-9.0, 0.5, 256).round(6).tolist(), "files": ["scales.webp"]},
        "quats": {"files": ["quats.webp"]},
        "sh0": {"codebook": np.linspace(-2.0, 4.0, 256).round(6).tolist(), "files": ["sh0.webp"]},
    }
    if bands:
        coeffs = {1: 3, 2: 8, 3: 15}[bands]
        labels = rng.integers(0, palette, N, dtype=np.uint32)
        lab = np.zeros((N, 3), np.uint8)
        lab[:, 0] = labels & 0xFF
        lab[:, 1] = labels >> 8
        rows = (palette + 63) // 64
        cent = rng.integers(0, 256, (rows, 64 * coeffs, 3), dtype=np.uint16).astype(np.uint8)
        files["shN_labels.webp"] = webp(texels(lab))
        files["shN_centroids.webp"] = webp(cent)
        meta["shN"] = {"count": palette, "bands": bands, "codebook": np.linspace(-0.8, 0.8, 256).round(6).tolist(),
                       "files": ["shN_centroids.webp", "shN_labels.webp"]}
    return meta, files


def write_zip(path, meta, files, method=zipfile.ZIP_STORED, extra_meta_text=None):
    with zipfile.ZipFile(path, "w", compression=method) as z:
        z.writestr("meta.json", extra_meta_text if extra_meta_text is not None else json.dumps(meta))
        for name, data in files.items():
            z.writestr(name, data)


def expected_records(path):
    dec = sd.decode(path)
    return sd.pack_records(dec).astype(np.uint32).tolist()


def main(out):
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    manifest = {}

    def add(name, error, meta, files, **kw):
        path = os.path.join(out, name + ".sog")
        write_zip(path, meta, files, **kw)
        entry = {"path": name + ".sog", "error": error}
        if error == "Ok":
            entry["records"] = expected_records(path)
        manifest[name] = entry

    meta, files = base_asset(bands=1)
    add("valid_stored", "Ok", meta, files)
    add("valid_deflate", "Ok", meta, files, method=zipfile.ZIP_DEFLATED)
    folder = os.path.join(out, "valid_folder")
    os.makedirs(folder)
    with open(os.path.join(folder, "meta.json"), "w") as f:
        json.dump(meta, f)
    for name, data in files.items():
        with open(os.path.join(folder, name), "wb") as f:
            f.write(data)
    manifest["valid_folder"] = {"path": "valid_folder", "error": "Ok", "records": manifest["valid_stored"]["records"]}

    m3, f3 = base_asset(bands=3, palette=130)             # 3 centroid rows, full band-3 layout
    add("valid_bands3", "Ok", m3, f3)
    m0, f0 = base_asset(bands=0)
    add("valid_no_sh", "Ok", m0, f0)

    bad = json.loads(json.dumps(meta))
    bad["version"] = 1
    add("bad_version", "BadVersion", bad, files)
    add("bad_json", "BadMeta", meta, files, extra_meta_text="{\"version\": 2, \"count\": ")
    bad = json.loads(json.dumps(meta))
    bad["scales"]["codebook"] = bad["scales"]["codebook"][:255]
    add("short_codebook", "BadMeta", bad, files)

    lossy = dict(files)
    sh0 = np.asarray(Image.open(io.BytesIO(files["sh0.webp"])).convert("RGBA"))
    lossy["sh0.webp"] = webp(sh0, lossless=False)
    add("lossy_image", "LossyImage", meta, lossy)

    q = np.asarray(Image.open(io.BytesIO(files["quats.webp"])).convert("RGBA")).copy()
    q.reshape(-1, 4)[17, 3] = 200
    badq = dict(files)
    badq["quats.webp"] = webp(q)
    add("bad_quat_mode", "BadQuatMode", meta, badq)

    lab = np.asarray(Image.open(io.BytesIO(files["shN_labels.webp"])).convert("RGB")).copy()
    lab.reshape(-1, 3)[42, :2] = [8, 0]                   # palette has 8 entries -> label 8 is out of range
    badl = dict(files)
    badl["shN_labels.webp"] = webp(lab)
    add("label_out_of_range", "LabelOutOfRange", meta, badl)

    small = dict(files)
    small["scales.webp"] = webp(np.zeros((4, 4, 3), np.uint8))
    add("image_too_small", "ImageSizeMismatch", meta, small)

    missing = {k: v for k, v in files.items() if k != "scales.webp"}
    add("missing_image", "MissingImage", meta, missing)

    with open(os.path.join(out, "not_a_zip.sog"), "wb") as f:
        f.write(b"this is not a zip archive" * 10)
    manifest["not_a_zip"] = {"path": "not_a_zip.sog", "error": "NotZipOrFolder"}

    with open(os.path.join(out, "fixtures.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"wrote {len(manifest)} fixtures to {out}")


if __name__ == "__main__":
    main(sys.argv[1])
