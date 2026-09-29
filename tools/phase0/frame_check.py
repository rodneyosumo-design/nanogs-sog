"""Check that transforming the file-frame covariance by fileToLocal (the SOG shader plan) gives the
same covariance as NanoGS's import-time PLY conversion (quat (w,x,y,z) -> (w,-z,-x,y), scales (sz,sx,sy)).

usage: .venv/bin/python frame_check.py <asset.sog> <out.json>
"""
import json
import sys

import numpy as np

import sog_decode as sd

# file (x, y, z) -> NanoGS local (z, x, -y); the x100 metres->cm factor is a uniform scale and is dropped
M = np.array([[0.0, 0.0, 1.0],
              [1.0, 0.0, 0.0],
              [0.0, -1.0, 0.0]])


def quat_to_matrix(w, x, y, z):
    """Same formula as NanoGS QuatToMatrix (GaussianDataTypes.ush)."""
    return np.stack([
        np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)], -1),
        np.stack([2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)], -1),
        np.stack([2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)], -1),
    ], -2)


def covariance(R, s):
    RS = R * s[:, None, :]
    return RS @ RS.transpose(0, 2, 1)


def main(sog_path, out_path):
    dec = sd.decode(sog_path, with_sh=False)
    rng = np.random.default_rng(7)
    idx = rng.choice(dec["count"], 200_000, replace=False)
    q = dec["quat_wxyz"][idx]
    q = q / np.linalg.norm(q, axis=1, keepdims=True)
    s = dec["scale"][idx]
    w, x, y, z = q.T

    cov_file = covariance(quat_to_matrix(w, x, y, z), s)
    cov_sog_path = M @ cov_file @ M.T                                   # plan: fileToLocal applied in shader
    cov_nanogs = covariance(quat_to_matrix(w, -z, -x, y), s[:, [2, 0, 1]])  # NanoGS PLY importer conversion

    scale2 = np.maximum((s ** 2).max(1), 1e-30)[:, None, None]
    rel = np.abs(cov_sog_path - cov_nanogs) / scale2
    pos_ok = np.allclose(M @ np.array([1.0, 2.0, 3.0]), [3.0, 1.0, -2.0])
    res = {
        "splats_checked": int(len(idx)),
        "max_relative_covariance_difference": float(rel.max()),
        "p99_relative_covariance_difference": float(np.percentile(rel.reshape(len(idx), -1).max(1), 99)),
        "position_mapping_matches_nanogs": bool(pos_ok),
        "det_fileToLocal": float(np.linalg.det(M)),
        "fileToLocal_column_major_cm": [0, 100, 0, 0, 0, 0, -100, 0, 100, 0, 0, 0, 0, 0, 0, 1],
    }
    print(res)
    with open(out_path, "w") as f:
        json.dump(res, f, indent=2)


if __name__ == "__main__":
    main(*sys.argv[1:3])
