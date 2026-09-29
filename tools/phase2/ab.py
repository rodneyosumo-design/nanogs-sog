#!/usr/bin/env python3
"""A/B captures and metrics against the actors from editor_setup.py.

  ab.py capture OUT.png ACTOR x y z pitch yaw roll   show one actor, place the viewport camera, capture (MCP)
  ab.py compare A.png B.png [DIFF.png]               PSNR: full frame, content box, content pixels
  ab.py views OUTDIR                                  the three Phase 2 views for all three actors, then the table
  ab.py profile LABEL ACTOR [cvar=value ...]          median NanoGS GPU times over N ProfileGPU frames (camera sways)

Needs the editor's MCP server (ModelContextProtocol.StartServer) and Python remote execution (see README.md).
Env: LOG = editor log file (profile), N = frames to profile (default 7), SWAY_AT = "x,y,z,pitch,yaw" camera the
profile sways around (default: the Phase 2 test-actor view; the SHUCampusLevel quad view is 2541,-2611,872,-4,132).
With ACTOR = "-" the profile leaves actor visibility alone (a real level)."""
import base64, json, os, re, statistics, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import uepy

VIEWS = {"v1": (-1600, -1300, 700, -18, 42, 0), "v2": (-250, -150, 40, -4, 42, 0), "v3": (100, 300, 10, 2, -140, 0)}
ACTORS = {"legacy": "NanoGS_A_Legacy", "sog": "NanoGS_B_SOG", "rt": "NanoGS_C_RoundtripLegacy"}

def mcp(toolset, tool, args):
    out = subprocess.run([os.path.join(HERE, "mcp.sh"), "call", toolset, tool, json.dumps(args)], capture_output=True,
                         text=True, env=dict(os.environ, MCP_TIMEOUT="300")).stdout
    txt = json.loads(out)["result"]["content"][0]["text"]
    try:
        return json.loads(txt)
    except ValueError:
        raise SystemExit("MCP error: " + txt[:500])

def show(actor):
    uepy.run("""import unreal
for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if a.get_actor_label().startswith("NanoGS_"):
        a.set_is_temporarily_hidden_in_editor(a.get_actor_label() != %r)
""" % actor)

def console(*cmds):
    uepy.run("import unreal\nw = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
             + "".join("unreal.SystemLibrary.execute_console_command(w, %r)\n" % c for c in cmds))

def capture(out, actor, x, y, z, pitch, yaw, roll):
    show(actor)
    uepy.run("import unreal\nunreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info("
             "unreal.Vector(%f, %f, %f), unreal.Rotator(roll=%f, pitch=%f, yaw=%f))" % (x, y, z, roll, pitch, yaw))
    cam = mcp("EditorToolset.EditorAppToolset", "GetCameraTransform", {})["returnValue"]
    rv = mcp("EditorToolset.EditorAppToolset", "CaptureViewport", {"captureTransform": cam, "annotations": [], "bShowUI": False})["returnValue"]
    open(out, "wb").write(base64.b64decode(rv["image"]["data"] if "image" in rv else rv["data"]))

def compare(a_path, b_path, diff_path=None):
    import numpy as np
    from PIL import Image
    load = lambda p: np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
    a, b = load(a_path), load(b_path)
    psnr = lambda x, y: 10 * np.log10(255.0 ** 2 / max(np.mean((x - y) ** 2), 1e-12))
    mask = (a.mean(axis=2) > 40) | (b.mean(axis=2) > 40)          # content: brighter than the dark background
    ys, xs = np.nonzero(mask)
    box = (slice(ys.min(), ys.max() + 1), slice(xs.min(), xs.max() + 1))
    if diff_path:
        d = np.clip(np.abs(a - b).max(axis=2) * 4, 0, 255).astype(np.uint8)
        Image.fromarray(d).save(diff_path)
    return "full frame %.2f dB, content box %.2f dB, content pixels %.2f dB, mean |diff| %.2f" % (
        psnr(a, b), psnr(a[box], b[box]), psnr(a[mask], b[mask]), np.abs(a - b)[mask].mean())

def views(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, cam in VIEWS.items():
        for key, actor in ACTORS.items():
            capture(os.path.join(outdir, "%s_%s.png" % (name, key)), actor, *cam)
        p = lambda k: os.path.join(outdir, "%s_%s.png" % (name, k))
        print("%s SOG GPU vs NanoGS path, same splats:  %s" % (name, compare(p("rt"), p("sog"))))
        print("%s original vs round trip (SOG error):   %s" % (name, compare(p("legacy"), p("rt"))))
        print("%s original vs SOG GPU:                  %s" % (name, compare(p("legacy"), p("sog"))))

# Each remote run gets fresh globals, so the tick handle lives in a module that persists in the editor
SWAY = """import math, sys, types, unreal
state = sys.modules.setdefault("_nanogs_phase2", types.ModuleType("_nanogs_phase2"))
if getattr(state, "sway", None) is not None:
    unreal.unregister_slate_post_tick_callback(state.sway)
    state.sway = None
if %(on)s:
    st = {"t": 0.0}
    def _tick(dt):
        st["t"] += dt
        s = math.sin(st["t"] * 2.0)
        unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(
            unreal.Vector(%(x)f + %(r)f * s, %(y)f + %(r)f * s, %(z)f), unreal.Rotator(roll=0, pitch=%(pitch)f, yaw=%(yaw)f + 2 * s))
    state.sway = unreal.register_slate_post_tick_callback(_tick)
"""

def profile(label, actor, cvars):
    log = os.environ.get("LOG") or max((os.path.join(d, f) for d in [os.path.expanduser("~/Library/Logs/Unreal Engine/SHUTourDemoEditor")]
                                        for f in os.listdir(d) if f.endswith(".log")), key=os.path.getmtime)
    n = int(os.environ.get("N", "7"))
    x, y, z, pitch, yaw = [float(v) for v in os.environ.get("SWAY_AT", "-250,-150,40,-4,42").split(",")]
    sway = {"on": True, "x": x, "y": y, "z": z, "pitch": pitch, "yaw": yaw, "r": max(20.0, abs(z) * 0.07)}
    if actor != "-":
        show(actor)
    uepy.run(SWAY % sway)                  # NanoGS skips view data and sort while the camera is still
    if cvars:
        console(*[c.replace("=", " ") for c in cvars])
    time.sleep(3)
    start = os.path.getsize(log)
    for _ in range(n):
        console("ProfileGPU")
        time.sleep(1.5)
    time.sleep(2)
    uepy.run(SWAY % dict(sway, on=False))
    text = open(log, "rb").read()[start:].decode("utf-8", "replace")
    stats = ("NanoGSViewData", "NanoGSSort", "NanoGSDraw", "NanoGSComposite")
    res = {k: [] for k in stats}
    for frame in text.split("GPU Profile for Frame")[1:]:
        for k in stats:
            # whole table rows; a stat can appear in several sibling scopes, so sum their inclusive times
            rows = re.findall(r"^.*┃\s+%s\s.*$" % k, frame, re.M)
            times = [float(re.findall(r"([\d.]+) ms", r)[1]) for r in rows if len(re.findall(r"([\d.]+) ms", r)) >= 2]
            if times:
                res[k].append(sum(times))
    med = {k: statistics.median(v) if v else float("nan") for k, v in res.items()}
    print("%-34s prepare %6.2f  sort %5.2f  draw %6.2f  composite %5.2f  total %6.2f ms  (n=%d)" % (
        label, med["NanoGSViewData"], med["NanoGSSort"], med["NanoGSDraw"], med["NanoGSComposite"], sum(med.values()), len(res["NanoGSDraw"])))

if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "capture":
        capture(args[0], args[1], *[float(v) for v in args[2:8]])
    elif cmd == "compare":
        print(compare(*args[:3]))
    elif cmd == "views":
        views(args[0])
    elif cmd == "profile":
        profile(args[0], args[1], args[2:])
    else:
        raise SystemExit(__doc__)
