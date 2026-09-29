"""Write binary test inputs and float64-reference expectations for the blueprint tests.

usage: ../.venv/bin/python export_test_data.py <asset.sog> <out_dir>
Outputs: records.bin (n x 20 B), texels.bin (n x 24 B: means_l, means_u, quats, scales, sh0, labels RGBA),
constants.bin (112 B), scale_lut.bin, dc_lut.bin (256 f32), palette_half4.bin, dir.bin (float4),
expected.bin (n x 24 f32: pos3 quat4 scale3 opacity color_dc3 sh_color3 label cov6).
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import sog_decode as sd  # noqa: E402

NANOGS_FILE_TO_LOCAL = [0, 100, 0, 0, 0, 0, -100, 0, 100, 0, 0, 0, 0, 0, 0, 1]  # column-major
C1 = 0.4886025119029199
C2 = [1.0925484305920792, -1.0925484305920792, 0.31539156525252005, -1.0925484305920792, 0.5462742152960396]
C3 = [-0.5900435899266435, 2.890611442640554, -0.4570457994644658, 0.3731763325901154,
      -0.4570457994644658, 1.445305721320277, -0.5900435899266435]


def sh_eval(dc_color, sh, d):
    """Reference 3DGS SH evaluation (float64). sh: (n, 15, 3) AC coefficients, d: unit direction."""
    x, y, z = d
    xx, yy, zz, xy, yz, xz = x * x, y * y, z * z, x * y, y * z, x * z
    basis = np.array([
        -C1 * y, C1 * z, -C1 * x,
        C2[0] * xy, C2[1] * yz, C2[2] * (2 * zz - xx - yy), C2[3] * xz, C2[4] * (xx - yy),
        C3[0] * y * (3 * xx - yy), C3[1] * xy * z, C3[2] * y * (4 * zz - xx - yy),
        C3[3] * z * (2 * zz - 3 * xx - 3 * yy), C3[4] * x * (4 * zz - xx - yy), C3[5] * z * (xx - yy),
        C3[6] * x * (xx - 3 * yy)])
    return np.maximum(dc_color + np.einsum("k,nkc->nc", basis, sh), 0.0)


def main(sog_path, out):
    os.makedirs(out, exist_ok=True)
    dec = sd.decode(sog_path)
    meta, n = dec["meta"], dec["count"]
    rec = sd.pack_records(dec)
    rec.astype("<u4").tofile(os.path.join(out, "records.bin"))

    raw = dec["raw"]
    texels = np.concatenate([raw[k] for k in ("means_l", "means_u", "quats", "scales", "sh0", "labels")], 1)
    texels.astype(np.uint8).tofile(os.path.join(out, "texels.bin"))

    coeffs = dec["sh_coeffs"]
    const = struct.pack("<16f3fI3fI4I", *NANOGS_FILE_TO_LOCAL, *meta["means"]["mins"], n,
                        *meta["means"]["maxs"], coeffs, int(bool(meta.get("antialias", False))), 0, 0, 0)
    assert len(const) == 112
    open(os.path.join(out, "constants.bin"), "wb").write(const)

    scale_lut, dc_lut = sd.build_tables(meta)
    scale_lut.tofile(os.path.join(out, "scale_lut.bin"))
    dc_lut.tofile(os.path.join(out, "dc_lut.bin"))
    pal = np.zeros(dec["palette"].shape[:2] + (4,), np.float16)
    pal[..., :3] = dec["palette"]
    pal.tofile(os.path.join(out, "palette_half4.bin"))

    d = np.array([0.3, -0.5, 0.8])
    d = d / np.linalg.norm(d)
    np.array([*d, 0.0], np.float32).tofile(os.path.join(out, "dir.bin"))

    M = np.array(NANOGS_FILE_TO_LOCAL, np.float64).reshape(4, 4).T[:3, :3]   # column-major -> matrix
    exp = np.zeros((n, 24), np.float32)
    exp[:, 0:3] = dec["pos"]
    exp[:, 3:7] = dec["quat_wxyz"]
    exp[:, 7:10] = dec["scale"]
    exp[:, 10] = dec["opacity"]
    color_dc = 0.5 + sd.SH_C0 * dec["dc"]
    exp[:, 11:14] = np.maximum(color_dc, 0.0)   # sog_color clamps negative colour
    exp[:, 17] = dec["labels"]
    for s in range(0, n, 250_000):
        e = slice(s, min(n, s + 250_000))
        sh = dec["palette"][dec["labels"][e]].astype(np.float64)
        exp[e, 14:17] = sh_eval(color_dc[e], sh, d)
        w, x, y, z = dec["quat_wxyz"][e].T
        R = np.stack([
            np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)], -1),
            np.stack([2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)], -1),
            np.stack([2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)], -1)], -2)
        MRS = M @ (R * dec["scale"][e][:, None, :])
        cov = MRS @ MRS.transpose(0, 2, 1)
        exp[e, 18:24] = np.stack([cov[:, 0, 0], cov[:, 0, 1], cov[:, 0, 2], cov[:, 1, 1], cov[:, 1, 2], cov[:, 2, 2]], 1)
    exp.tofile(os.path.join(out, "expected.bin"))
    print(f"wrote {n} splats to {out}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
