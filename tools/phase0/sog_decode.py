"""Reference decoder for PlayCanvas SOG v2 (Spatially Ordered Gaussians).

Spec: https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/

This is the golden implementation the C++ importer and the Metal/HLSL decode path are
tested against. It also implements the 20-byte GPU record from blueprint/SOGTypes.h and a
float32 decoder that mirrors blueprint/SOGDecode.metal line for line.
"""
import io
import json
import os
import zipfile

import numpy as np
from PIL import Image

SH_C0 = 0.28209479177387814
SH_COEFFS = {1: 3, 2: 8, 3: 15}


class SogSource:
    """A bundled .sog (zip), an unbundled folder, or a path to its meta.json."""

    def __init__(self, path):
        self.zip = None
        if os.path.isdir(path):
            self.dir = path
        elif os.path.basename(path) == "meta.json":
            self.dir = os.path.dirname(path)
        else:
            self.zip = zipfile.ZipFile(path)
            self.dir = None

    def read(self, name):
        if self.zip is not None:
            return self.zip.read(name)
        with open(os.path.join(self.dir, name), "rb") as f:
            return f.read()


def webp_is_lossless(data):
    """True for VP8L (lossless) payloads, False for VP8 (lossy), including VP8X-wrapped files."""
    if data[:4] != b"RIFF" or data[8:12] != b"WEBP":
        raise ValueError("not a WebP file")
    pos = 12
    while pos + 8 <= len(data):
        fourcc = data[pos:pos + 4]
        size = int.from_bytes(data[pos + 4:pos + 8], "little")
        if fourcc == b"VP8L":
            return True
        if fourcc == b"VP8 ":
            return False
        pos += 8 + size + (size & 1)
    raise ValueError("WebP has no image chunk")


def read_rgba(src, name, count=None):
    """Exact RGBA8 texels (straight alpha). Returns (H, W, 4), or (count, 4) when count is given."""
    data = src.read(name)
    if not webp_is_lossless(data):
        raise ValueError(f"{name} is lossy WebP; SOG requires lossless images")
    im = Image.open(io.BytesIO(data))
    im.load()
    arr = np.asarray(im.convert("RGBA"))
    if count is None:
        return arr
    h, w = arr.shape[:2]
    if w * h < count:
        raise ValueError(f"{name} is {w}x{h}, smaller than count={count}")
    return arr.reshape(-1, 4)[:count]


def decode(path, with_sh=True):
    """Decode a SOG v2 asset to float64 attributes plus the raw texels used to build GPU records."""
    src = SogSource(path)
    meta = json.loads(src.read("meta.json"))
    if meta.get("version") != 2:
        raise ValueError(f"unsupported SOG version {meta.get('version')}")
    n = int(meta["count"])

    ml = read_rgba(src, meta["means"]["files"][0], n)
    mu = read_rgba(src, meta["means"]["files"][1], n)
    q16 = (mu[:, :3].astype(np.uint32) << 8) | ml[:, :3]
    mins = np.asarray(meta["means"]["mins"], np.float64)
    maxs = np.asarray(meta["means"]["maxs"], np.float64)
    nlog = mins + (maxs - mins) * (q16 / 65535.0)
    pos = np.sign(nlog) * np.expm1(np.abs(nlog))

    qt = read_rgba(src, meta["quats"]["files"][0], n)
    mode = qt[:, 3].astype(np.int32) - 252
    if mode.min() < 0 or mode.max() > 3:
        raise ValueError("quats alpha outside 252..255")
    abc = (qt[:, :3] / 255.0 - 0.5) * np.sqrt(2.0)
    d = np.sqrt(np.maximum(0.0, 1.0 - (abc * abc).sum(1)))
    quat = np.empty((n, 4))
    for m in range(4):
        idx = np.nonzero(mode == m)[0]
        others = [i for i in range(4) if i != m]
        quat[idx, m] = d[idx]
        quat[idx[:, None], others] = abc[idx]

    sc = read_rgba(src, meta["scales"]["files"][0], n)
    scale_log = np.asarray(meta["scales"]["codebook"], np.float64)[sc[:, :3]]

    s0 = read_rgba(src, meta["sh0"]["files"][0], n)
    dc = np.asarray(meta["sh0"]["codebook"], np.float64)[s0[:, :3]]
    opacity = s0[:, 3] / 255.0

    out = {
        "meta": meta, "count": n, "pos": pos, "quat_wxyz": quat,
        "scale_log": scale_log, "scale": np.exp(scale_log), "opacity": opacity, "dc": dc,
        "raw": {"means_l": ml, "means_u": mu, "quats": qt, "scales": sc, "sh0": s0},
    }
    if with_sh and "shN" in meta:
        shn = meta["shN"]
        coeffs = SH_COEFFS[int(shn["bands"])]
        count = int(shn["count"])
        cent = read_rgba(src, shn["files"][0])
        lab = read_rgba(src, shn["files"][1], n)
        labels = lab[:, 0].astype(np.uint32) | (lab[:, 1].astype(np.uint32) << 8)
        if labels.max() >= count:
            raise ValueError("shN label out of range")
        rows = cent.shape[0]
        # entry e lives in row e // 64, columns (e % 64) * coeffs + c
        pal_idx = cent[:, :64 * coeffs, :3].reshape(rows, 64, coeffs, 3).reshape(rows * 64, coeffs, 3)[:count]
        palette = np.asarray(shn["codebook"], np.float32)[pal_idx]          # (count, coeffs, 3)
        out.update(labels=labels, palette=palette, sh_coeffs=coeffs, sh_bands=int(shn["bands"]))
        out["raw"]["labels"] = lab
    return out


# ---- GPU layout (blueprint/SOGTypes.h) ------------------------------------------------------

def pack_records(dec):
    """(count, 5) uint32 records, byte-identical to sog::PackSplat."""
    raw = dec["raw"]
    ml, mu, qt, sc, s0 = (raw[k].astype(np.uint32) for k in ("means_l", "means_u", "quats", "scales", "sh0"))
    x = ml[:, 0] | (mu[:, 0] << 8)
    y = ml[:, 1] | (mu[:, 1] << 8)
    z = ml[:, 2] | (mu[:, 2] << 8)
    label = dec["labels"].astype(np.uint32) if "labels" in dec else np.zeros(dec["count"], np.uint32)
    rec = np.empty((dec["count"], 5), np.uint32)
    rec[:, 0] = x | (y << 16)
    rec[:, 1] = z | (label << 16)
    rec[:, 2] = qt[:, 0] | (qt[:, 1] << 8) | (qt[:, 2] << 16) | ((qt[:, 3] - 252) << 24)
    rec[:, 3] = sc[:, 0] | (sc[:, 1] << 8) | (sc[:, 2] << 16) | (s0[:, 3] << 24)
    rec[:, 4] = s0[:, 0] | (s0[:, 1] << 8) | (s0[:, 2] << 16)
    return rec


def build_tables(meta):
    """Import-time tables: scaleLUT = exp(codebook), dcLUT = 0.5 + SH_C0 * codebook (float32)."""
    scale_lut = np.exp(np.asarray(meta["scales"]["codebook"], np.float32)).astype(np.float32)
    dc_lut = (np.float32(0.5) + np.float32(SH_C0) * np.asarray(meta["sh0"]["codebook"], np.float32)).astype(np.float32)
    return scale_lut, dc_lut


def gpu_decode(rec, meta, scale_lut, dc_lut):
    """float32 decode mirroring blueprint/SOGDecode.metal (sog_mean, sog_quat_wxyz, sog_scale, ...)."""
    f = np.float32
    q = np.stack([rec[:, 0] & 0xFFFF, rec[:, 0] >> 16, rec[:, 1] & 0xFFFF], 1).astype(f) * f(1.0 / 65535.0)
    mn = np.asarray(meta["means"]["mins"], f)
    mx = np.asarray(meta["means"]["maxs"], f)
    n = mn + (mx - mn) * q                                   # mix(meanMin, meanMax, q)
    pos = np.sign(n) * (np.exp(np.abs(n)) - f(1.0))

    p = rec[:, 2]
    abc = (np.stack([p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF], 1).astype(f) * f(1.0 / 255.0) - f(0.5)) * f(np.sqrt(2.0))
    d = np.sqrt(np.maximum(f(0.0), f(1.0) - (abc * abc).sum(1)))
    mode = p >> 24
    quat = np.empty((len(rec), 4), f)
    for m in range(4):
        idx = np.nonzero(mode == m)[0]
        others = [i for i in range(4) if i != m]
        quat[idx, m] = d[idx]
        quat[idx[:, None], others] = abc[idx]

    s = rec[:, 3]
    scale = np.stack([scale_lut[s & 0xFF], scale_lut[(s >> 8) & 0xFF], scale_lut[(s >> 16) & 0xFF]], 1)
    opacity = (s >> 24).astype(f) * f(1.0 / 255.0)
    c = rec[:, 4]
    color_dc = np.stack([dc_lut[c & 0xFF], dc_lut[(c >> 8) & 0xFF], dc_lut[(c >> 16) & 0xFF]], 1)
    label = rec[:, 1] >> 16
    return {"pos": pos, "quat_wxyz": quat, "scale": scale, "opacity": opacity, "color_dc": color_dc, "label": label}


# ---- PLY ------------------------------------------------------------------------------------

_PLY_TYPES = {"float": "<f4", "double": "<f8", "uchar": "u1", "char": "i1", "short": "<i2",
              "ushort": "<u2", "int": "<i4", "uint": "<u4"}


def load_ply(path):
    """Memory-mapped binary little-endian 3DGS PLY as a numpy structured array."""
    with open(path, "rb") as f:
        hdr = b""
        while not hdr.endswith(b"end_header\n"):
            line = f.readline()
            if not line:
                raise ValueError("PLY header not terminated")
            hdr += line
        off = f.tell()
    props, n = [], 0
    for line in hdr.decode("ascii").splitlines():
        p = line.split()
        if p[:1] == ["format"] and p[1] != "binary_little_endian":
            raise ValueError("only binary_little_endian PLY is supported")
        if p[:2] == ["element", "vertex"]:
            n = int(p[2])
        if p[:1] == ["property"]:
            props.append((p[2], _PLY_TYPES[p[1]]))
    return np.memmap(path, dtype=np.dtype(props), mode="r", offset=off, shape=(n,))


def ply_sh_rest(ply, coeffs=15):
    """PLY f_rest is channel-planar (f_rest_0..14 = R). Returns (n, coeffs, 3)."""
    cols = [ply[f"f_rest_{i}"] for i in range(3 * coeffs)]
    return np.stack(cols, 1).reshape(len(ply), 3, coeffs).transpose(0, 2, 1)
