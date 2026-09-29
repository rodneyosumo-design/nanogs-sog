#!/usr/bin/env python3
"""Run a Python file inside the running Unreal Editor over Python remote execution.
usage: uepy.py script.py
The editor needs remote execution on with bind address 0.0.0.0 (see README.md). UE_ROOT points at the engine
(default: the SHUTourDemo engine install) for its remote_execution.py client."""
import os, sys, tempfile, time
UE_ROOT = os.environ.get("UE_ROOT", "/Volumes/External SSD/Epic Games/UE_5.8")
sys.path.insert(0, os.path.join(UE_ROOT, "Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python"))
import remote_execution as rex_mod

def run(code):
    cfg = rex_mod.RemoteExecutionConfig()
    cfg.multicast_bind_address = "0.0.0.0"
    rex = rex_mod.RemoteExecution(cfg)
    rex.start()
    try:
        node = None
        for _ in range(50):
            if rex.remote_nodes:
                node = rex.remote_nodes[0]["node_id"]
                break
            time.sleep(0.2)
        if not node:
            raise SystemExit("no editor found (is remote execution enabled?)")
        rex.open_command_connection(node)
        # Pass a file: the editor treats a literal script whose first line mentions a .py file as a path
        with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
            f.write(code)
        try:
            res = rex.run_command(f.name, unattended=True, exec_mode=rex_mod.MODE_EXEC_FILE)
        finally:
            os.unlink(f.name)
        out = "\n".join(o.get("output", "").rstrip() for o in res.get("output", []))
        if not res.get("success"):
            raise SystemExit(out + "\nFAILED: " + str(res.get("result")))
        return out
    finally:
        rex.stop()

if __name__ == "__main__":
    print(run(open(sys.argv[1]).read()))
