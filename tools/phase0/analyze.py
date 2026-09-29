"""Phase 0 analysis: validate the reference decoder and GPU layout, confirm the coordinate frame,
and measure SOG's error against the original PLY.

usage: .venv/bin/python analyze.py <original.ply> <asset.sog> <roundtrip.ply> <out_dir>
"""
import json
import os
import sys
import time

import numpy as np
from scipy.spatial import cKDTree

import sog_decode as sd


def pct(a, qs=(50, 95, 99, 99.9)):
    a = np.asarray(a, np.float64)
    return {f"p{q}": float(np.percentile(a, q)) for q in qs} | {"max": float(a.max()), "mean": float(a.mean())}


def unit_quat(q):
    return q / np.linalg.norm(q, axis=1, keepdims=True)


def main(orig_path, sog_path, rt_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    res = {}
    t0 = time.time()
    dec = sd.decode(sog_path)
    meta = dec["meta"]
    n = dec["count"]
    orig = sd.load_ply(orig_path)
    rt = sd.load_ply(rt_path)
    res["counts"] = {"original_ply": int(len(orig)), "sog": n, "roundtrip_ply": int(len(rt))}
    res["sizes_bytes"] = {"original_ply": os.path.getsize(orig_path), "sog": os.path.getsize(sog_path)}
    res["sizes_bytes"]["sog_bytes_per_splat"] = res["sizes_bytes"]["sog"] / n
    res["sizes_bytes"]["ply_bytes_per_splat"] = res["sizes_bytes"]["original_ply"] / len(orig)
    res["meta"] = {k: v for k, v in meta.items() if k not in ("scales", "sh0", "shN")}
    res["meta"]["shN"] = {k: v for k, v in meta.get("shN", {}).items() if k != "codebook"}
    print(f"decoded {n} splats in {time.time() - t0:.1f}s")

    # 1. Reference decoder vs splat-transform's own decode (round-trip PLY is in SOG order).
    rt_pos = np.stack([rt["x"], rt["y"], rt["z"]], 1).astype(np.float64)
    rt_q = unit_quat(np.stack([rt[f"rot_{i}"] for i in range(4)], 1).astype(np.float64))
    my_q = unit_quat(dec["quat_wxyz"])
    rt_scale = np.stack([rt[f"scale_{i}"] for i in range(3)], 1).astype(np.float64)
    rt_opacity = 1.0 / (1.0 + np.exp(-rt["opacity"].astype(np.float64)))
    rt_dc = np.stack([rt[f"f_dc_{i}"] for i in range(3)], 1).astype(np.float64)
    v = {
        "pos_max_abs": float(np.abs(rt_pos - dec["pos"]).max()),
        "quat_max_abs(sign-aligned)": float(np.abs(np.abs((rt_q * my_q).sum(1)) - 1.0).max()),
        "scale_log_max_abs": float(np.abs(rt_scale - dec["scale_log"]).max()),
        "opacity_max_abs": float(np.abs(rt_opacity - dec["opacity"]).max()),
        "dc_max_abs": float(np.abs(rt_dc - dec["dc"]).max()),
    }
    if "palette" in dec:
        sh_err = 0.0
        for s in range(0, n, 250_000):
            e = slice(s, min(n, s + 250_000))
            sh_err = max(sh_err, float(np.abs(sd.ply_sh_rest(rt[e], dec["sh_coeffs"]) - dec["palette"][dec["labels"][e]]).max()))
        v["sh_rest_max_abs"] = sh_err
    res["decoder_vs_splat_transform"] = v
    print("decoder vs splat-transform:", v)

    # 2. GPU record layout + float32 decode vs the float64 reference.
    rec = sd.pack_records(dec)
    scale_lut, dc_lut = sd.build_tables(meta)
    g = sd.gpu_decode(rec, meta, scale_lut, dc_lut)
    ref_color = 0.5 + sd.SH_C0 * dec["dc"]
    gv = {
        "record_bytes": int(rec.itemsize * rec.shape[1]),
        "pos_max_abs": float(np.abs(g["pos"] - dec["pos"]).max()),
        "pos_max_rel": float((np.abs(g["pos"] - dec["pos"]) / np.maximum(1e-6, np.abs(dec["pos"]))).max()),
        "quat_max_abs": float(np.abs(g["quat_wxyz"] - dec["quat_wxyz"]).max()),
        "scale_max_rel": float((np.abs(g["scale"] - dec["scale"]) / dec["scale"]).max()),
        "opacity_max_abs": float(np.abs(g["opacity"] - dec["opacity"]).max()),
        "color_dc_max_abs": float(np.abs(g["color_dc"] - ref_color).max()),
        "labels_equal": bool((g["label"] == dec.get("labels", 0)).all()),
    }
    res["gpu_layout_vs_reference"] = gv
    print("gpu layout vs reference:", gv)

    # 3. Coordinate frame: bounds of the original PLY vs decoded SOG.
    o_pos = np.stack([orig["x"], orig["y"], orig["z"]], 1).astype(np.float64)
    res["frame"] = {
        "ply_bbox_min": o_pos.min(0).round(4).tolist(), "ply_bbox_max": o_pos.max(0).round(4).tolist(),
        "sog_bbox_min": dec["pos"].min(0).round(4).tolist(), "sog_bbox_max": dec["pos"].max(0).round(4).tolist(),
        "ply_centroid": o_pos.mean(0).round(4).tolist(), "sog_centroid": dec["pos"].mean(0).round(4).tolist(),
        "same_frame": bool(np.allclose(o_pos.min(0), dec["pos"].min(0), atol=0.01)
                           and np.allclose(o_pos.max(0), dec["pos"].max(0), atol=0.01)),
    }
    print("frame:", res["frame"])

    # 4. Fidelity: match every original splat to its SOG counterpart (SOG reorders by Morton code).
    t1 = time.time()
    tree_sog = cKDTree(dec["pos"])
    dist, idx = tree_sog.query(o_pos, k=1, workers=-1)
    back = cKDTree(o_pos).query(dec["pos"][idx], k=1, workers=-1)[1]
    mutual = back == np.arange(len(o_pos))
    res["matching"] = {"mutual_nearest_fraction": float(mutual.mean()),
                       "unique_sog_targets": int(len(np.unique(idx))), "seconds": round(time.time() - t1, 1)}
    print("matching:", res["matching"])
    o = np.nonzero(mutual)[0]
    s = idx[o]

    o_scale_log = np.stack([orig[f"scale_{i}"] for i in range(3)], 1).astype(np.float64)[o]
    o_q = unit_quat(np.stack([orig[f"rot_{i}"] for i in range(4)], 1).astype(np.float64)[o])
    o_op = 1.0 / (1.0 + np.exp(-orig["opacity"].astype(np.float64)[o]))
    o_dc = np.stack([orig[f"f_dc_{i}"] for i in range(3)], 1).astype(np.float64)[o]
    size = np.exp(o_scale_log).max(1)
    ang = np.degrees(2.0 * np.arccos(np.clip(np.abs((o_q * my_q[s]).sum(1)), 0.0, 1.0)))
    fid = {
        "position_error_mm": pct(dist[o] * 1000.0),
        "position_error_vs_splat_size": pct(dist[o] / np.maximum(size, 1e-9)),
        "scale_log_error": pct(np.abs(o_scale_log - dec["scale_log"][s]).max(1)),
        "rotation_error_deg": pct(ang),
        "rotation_error_deg_non_spherical": pct(ang[(np.exp(o_scale_log).max(1) / np.exp(o_scale_log).min(1)) > 1.5]),
        "opacity_error": pct(np.abs(o_op - dec["opacity"][s])),
        "dc_color_error_8bit_levels": pct(np.abs(sd.SH_C0 * (o_dc - dec["dc"][s])).max(1) * 255.0),
    }
    if "palette" in dec:
        c = dec["sh_coeffs"]
        rms_err, rms_mag = [], []
        for k in range(0, len(o), 250_000):
            oo = o[k:k + 250_000]
            ss = s[k:k + 250_000]
            a = sd.ply_sh_rest(orig[np.sort(oo)], c)[np.argsort(np.argsort(oo))]
            b = dec["palette"][dec["labels"][ss]]
            rms_err.append(np.sqrt(((a - b) ** 2).mean((1, 2))))
            rms_mag.append(np.sqrt((a ** 2).mean((1, 2))))
        rms_err = np.concatenate(rms_err)
        rms_mag = np.concatenate(rms_mag)
        fid["sh_rest_rms_error"] = pct(rms_err)
        fid["sh_rest_rms_magnitude"] = pct(rms_mag)
        fid["sh_rest_relative_error_where_significant"] = pct((rms_err / rms_mag)[rms_mag > 0.05])
    res["fidelity_original_vs_sog"] = fid
    for k, val in fid.items():
        print(f"  {k}: " + ", ".join(f"{kk}={vv:.4g}" for kk, vv in val.items()))

    # Outliers: how many splats does the fixed 256-entry codebooks' range actually clip?
    sc_cb = np.asarray(meta["scales"]["codebook"])
    dc_cb = np.asarray(meta["sh0"]["codebook"])
    dc_err = np.abs(sd.SH_C0 * (o_dc - dec["dc"][s])).max(1) * 255.0
    scale_err = np.abs(o_scale_log - dec["scale_log"][s]).max(1)
    out = {
        "matched_pairs": int(len(o)),
        "rotation_gt_10deg": int((ang > 10).sum()),
        "rotation_gt_10deg_with_near_zero_norm_source_quat": int(((ang > 10) & (np.linalg.norm(np.stack([orig[f"rot_{i}"] for i in range(4)], 1).astype(np.float64)[o], axis=1) < 1e-3)).sum()),
        "scale_log_error_gt_0.2": int((scale_err > 0.2).sum()),
        "scale_source_outside_codebook_range": int(((o_scale_log < sc_cb.min()) | (o_scale_log > sc_cb.max())).any(1).sum()),
        "dc_error_gt_8_levels": int((dc_err > 8).sum()),
        "dc_source_outside_codebook_range": int(((o_dc < dc_cb.min()) | (o_dc > dc_cb.max())).any(1).sum()),
        "opacity_error_gt_0.05": int((np.abs(o_op - dec["opacity"][s]) > 0.05).sum()),
    }
    res["outliers"] = out
    print("outliers:", out)

    # 5. GPU memory projection for the NanoGS SOG storage mode.
    pal_bytes = dec["palette"].shape[0] * dec["sh_coeffs"] * 8 if "palette" in dec else 0
    nanite_total = 3_432_502  # scene_nosky incl. NanoGS LOD splats (memory: 2026-09-03)
    res["gpu_memory_projection_MB"] = {
        "sog_records_base_splats": n * 20 / 1e6,
        "sog_records_incl_nanogs_lod": nanite_total * 20 / 1e6,
        "sh_palette_half4": pal_bytes / 1e6,
        "luts_and_constants": (256 * 4 * 2 + 112) / 1e6,
        "total_sog_mode": (nanite_total * 20 + pal_bytes) / 1e6,
        # NanoGS today: 16-byte packed core + Float16 SH (16 coeffs x RGB x 2 B = 96 B), per splat
        "nanogs_gpu_today_packed_core": nanite_total * 16 / 1e6,
        "nanogs_gpu_today_sh_float16": nanite_total * 96 / 1e6,
        "nanogs_gpu_today_total": nanite_total * 112 / 1e6,
        # cooked bulk payload read at load (Position 41 + Other 96 + SH 330 + Color 27 MB), memory 2026-09-03
        "nanogs_cooked_bulk_payload": 41 + 96 + 330 + 27,
    }
    print("memory:", res["gpu_memory_projection_MB"])

    # 6. Golden vectors for the Phase 1 C++/Metal unit tests.
    rng = np.random.default_rng(20260929)
    pick = set(rng.choice(n, 48, replace=False).tolist())
    for m in range(4):
        pick.add(int(np.nonzero((rec[:, 2] >> 24) == m)[0][0]))
    pick |= {0, n - 1, int(np.argmin(dec["pos"][:, 0])), int(np.argmax(dec["pos"][:, 0])), int(np.argmax(dec["scale"].max(1)))}
    pick = sorted(pick)
    golden = {
        "source": os.path.basename(sog_path), "spec_version": 2,
        "constants": {"count": n, "meanMin": meta["means"]["mins"], "meanMax": meta["means"]["maxs"],
                      "shCoeffs": dec.get("sh_coeffs", 0), "antialias": bool(meta.get("antialias", False))},
        "scaleLUT": scale_lut.tolist(), "dcLUT": dc_lut.tolist(),
        "splats": [],
    }
    for i in pick:
        entry = {
            "index": i, "record": [int(x) for x in rec[i]],
            "pos": dec["pos"][i].tolist(), "quat_wxyz": dec["quat_wxyz"][i].tolist(),
            "scale": dec["scale"][i].tolist(), "opacity": float(dec["opacity"][i]),
            "color_dc": ref_color[i].tolist(),
        }
        if "palette" in dec:
            entry["label"] = int(dec["labels"][i])
            entry["sh_rest"] = dec["palette"][dec["labels"][i]].astype(np.float64).round(7).tolist()
        golden["splats"].append(entry)
    with open(os.path.join(out_dir, "golden_scene_nosky.json"), "w") as f:
        json.dump(golden, f, indent=1)
    with open(os.path.join(out_dir, "phase0_results.json"), "w") as f:
        json.dump(res, f, indent=2)
    print(f"wrote {out_dir}/phase0_results.json and golden_scene_nosky.json ({len(pick)} splats) in {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main(*sys.argv[1:5])
