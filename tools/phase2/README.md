# Phase 2 tools: A/B renders and GPU timing in the Unreal editor

These drive a running SHUTourDemo editor through its MCP server (viewport captures) and Python remote
execution (everything else). Nothing is saved except the temporary assets under
`/Game/NanoGS_SOGTest_Temp`, which you delete afterwards.

1. Start the editor with the MCP server and remote execution (bind 0.0.0.0, or the macOS client never
   finds it). It opens the empty startup map:

   ```bash
   S='[/Script/PythonScriptPlugin.PythonScriptPluginSettings]'
   UnrealEditor SHUTourDemo.uproject -NoP4 -ExecCmds="ModelContextProtocol.StartServer" \
       "-ini:Engine:$S:bRemoteExecution=True" "-ini:Engine:$S:RemoteExecutionMulticastBindAddress=0.0.0.0" \
       "-ini:Engine:$S:RemoteExecutionMulticastTtl=0" -log=NanoGSPhase2.log
   ```

2. Connect and set up the three actors (shipping asset, SOG asset, SOG round trip through NanoGS's own path):

   ```bash
   tools/phase2/mcp.sh init
   python3 tools/phase2/uepy.py tools/phase2/editor_setup.py
   ```

   Turn off the editor's background throttling while measuring, and back on afterwards:
   `tools/phase2/mcp.sh call editor_toolset.toolsets.object.ObjectTools set_properties
   '{"instance":{"refPath":"/Script/UnrealEd.Default__EditorPerformanceSettings"},"values":"{\"bThrottleCPUWhenNotForeground\": false}"}'`

3. Measure (`ab.py` needs numpy and Pillow, e.g. the Phase 0 venv):

   ```bash
   PY=tools/phase0/.venv/bin/python
   $PY tools/phase2/ab.py views data/phase2_views          # three views x three actors, PSNR table
   $PY tools/phase2/ab.py profile "SOG, 16-bit keys" NanoGS_B_SOG gs.SortKeyBits=16
   $PY tools/phase2/ab.py profile "NanoGS, 32-bit keys" NanoGS_A_Legacy gs.SortKeyBits=32
   ```

   `profile` sways the camera (NanoGS skips view data and sort while the camera is still), runs
   `ProfileGPU` N times (default 7) and prints median inclusive times of the `NanoGS*` GPU stats.
   On Metal the whole prepare pass is one compute encoder, so its time lands on `NanoGSViewData`
   (culling, compaction, view data and sort) and `NanoGSSort` reads 0.

4. For the sort itself: `gs.ValidateSort 1` in the editor console logs one line per checked frame
   (`gs.ValidateSort: OK - N keys ...`).

5. Clean up: quit the editor without saving the level, then delete `Content/NanoGS_SOGTest_Temp`.
