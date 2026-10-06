# Phase 1 results: SOG import (2026-09-29)

**Outcome:** NanoGS imports `.sog` bundles and unbundled SOG folders as Gaussian splat assets.
- Each asset carries SOG's 20-byte GPU records next to NanoGS's existing buffers, so it renders today
  through the current pipeline and is ready for Phase 2's GPU decode.
- Verified inside the Unreal editor:
  - The SOG path produces exactly the splats NanoGS's own PLY importer produces.
  - The Nanite build keeps every record aligned with the legacy buffers.
  - Version-6 assets survive a save and reload.
  - Existing version-5 assets still load.

**Decision:** option A, the full 20-byte SOG record (chosen 2026-09-29).

The Unreal-side changes are in **pending Perforce changelist 363 (not submitted)**; a review snapshot is
in [integration/nanogs-phase1.patch](../integration/nanogs-phase1.patch).

## What was built

**This repo**

| Path | Role |
|---|---|
| `sog/SOGTypes.h` | 20-byte record, 112-byte constants, flags byte (bit 0 = no view-dependent SH), `SOG_API` export macro |
| `sog/SOGCodec.*` | Dependency-free CPU decode (double precision), GPU tables, IEEE half conversion, encoder for NanoGS's merged LOD splats |
| `sog/SOGLoader.*` | Loads a bundled `.sog` or an unbundled folder through a `FileSource` interface; validates everything the spec requires |
| `sog/SOGJson.*`, `sog/SOGZip.*` | Locale-independent JSON; zip reader (stored + deflate via zlib, CRC-checked) |
| `third_party/libwebp` | libwebp v1.6.0 decoder only (generic + SSE2/SSE4.1/AVX2 + NEON), BSD-3 |
| `tests/` | `run_tests.sh`: codec, fixtures and full-scene suites |
| `tools/phase1/make_fixtures.py` | 14 fixture files: valid (stored, deflate, folder, band 3, no SH) and one per error path |
| `tools/sync_nanogs.sh` | Copies the library into the plugin, or `--check`s the copies are identical |

**NanoGS** (changelist 363, 116 files)

- **Runtime module:**
  - The vendored codec (`Public/SOG`, `Private/SOG`).
  - `GaussianSplatSOG`: the reader hook, conversion to NanoGS's CPU splats with the PLY importer's exact axis conventions, and building records for Nanite-built assets.
  - The cluster builder now reports its Morton permutation.
- **Asset format version 6:** an optional SOG payload with records and half-float palette as bulk data, the lookup tables, and the constants. Assets without SOG data are still written as version 5, so older NanoGS builds load them.
- **Nanite build and clear:** both read SOG sources. SOG records follow the builder's reordering, and merged LOD splats are quantized against the source's ranges and codebooks.
- **Editor module:**
  - The vendored loader and libwebp, editor-only so nothing WebP ships to iOS. Unity builds are off because libwebp's C files reuse static helper names; zlib comes from the engine.
  - A reader using Unreal's file APIs, registered at startup.
  - The factory accepts `.sog`, plus `meta.json` files that look like SOG; it rejects other JSON.
  - A PLY reimport clears stale SOG data.
- **Automation tests:** four tests under `NanoGS.SOG.*`.

## Results

**Library** (`tests/run_tests.sh`, about 12 s):

| Suite | Result |
|---|---|
| Codec | All 65,536 halves round-trip; `FloatToHalf` matches the M4's hardware conversion on 16.8M floats; encoder corner cases pass |
| Fixtures | 14/14 give the expected result, and every valid fixture's records match the Python reference |
| Full scene | Loads 2,575,004 splats in **0.27 s**. Records, tables and half-float palette are byte-identical to Phase 0's reference. CPU decode matches `splat-transform`: positions exact, rotation 5×10⁻⁸, scale 6×10⁻⁸, opacity 3×10⁻⁸, colour and SH exact. Encoder reproduces every record (1.3% of quaternions pick a different dropped component, within 0.45°) |

**In the Unreal editor** (headless, `-nullrhi -NoP4`):

| Test | Result |
|---|---|
| `NanoGS.SOG.ReaderMatchesPLYImporter` | All 2,575,004 splats identical to NanoGS's PLY importer on the round-trip PLY: position, rotation, colour and SH exact; scale 1.2×10⁻⁷ relative, opacity 6×10⁻⁸ |
| `NanoGS.SOG.ImportNaniteSaveReload` | Records and palette byte-identical to the reference. Nanite build gives 2,575,004 base + 857,498 LOD splats in 22,994 clusters (same as `scene_nosky`). Base records match the legacy buffer exactly after reordering; LOD splats within 0.032 cm; all LOD splats flagged no-SH. Save/reload keeps records, palette, constants and clusters |
| `NanoGS.SOG.LegacyAssetStillLoads` | `scene_nosky` (version 5) loads: 3,432,502 splats, 22,994 clusters |
| `NanoGS.SOG.Fixtures` | 14/14 through Unreal's file APIs, plus importing an unbundled folder through the factory |

Timing: reading and converting the 2.57M-splat `.sog` takes about 1.5 s in the editor, and the Nanite
build about 20 s. The iOS game target compiles and links with the new runtime code.

**Memory, for now:** SOG assets currently carry both the legacy buffers (so they render) and the SOG
payload, which adds about 77 MB of cooked data for a `scene_nosky`-sized asset. Phase 2 drops the legacy
buffers for SOG assets.

## Deviations from the plan

- **LOD splats get a no-SH flag instead of an inherited palette label.** NanoGS's merged LOD splats have
  zero SH and identity rotation, so the shader just skips the palette for them.
- **Own zip reader instead of `FZipArchiveReader`.** The loader stays engine-independent, and the
  standalone tests exercise the same code the editor runs.
- **Streamed SOG (`lod-meta.json`) import moves to Phase 4**, alongside streaming.
- **No y-up frame option yet.** There's no y-up SOG to test with. `splat-transform` output uses the PLY
  frame (Phase 0); revisit with a SuperSplat export.

## Correction to Phase 0

`BuildNaniteClusterHierarchy` is `BlueprintCallable`, so the Nanite build *can* be scripted from Python.
The Phase 0 write-up said it was menu-only.

## Before submitting changelist 363

- **Win64 must be rebuilt on the Alienware.** The vendored libwebp and the new code haven't been compiled
  with MSVC yet; `sog/` builds warning-free with clang `-Wall -Wextra -Wshadow`.
- **Asset compatibility:** PLY-sourced assets stay version 5, so nothing existing changes. SOG assets are
  version 6 and need the new binaries on every machine that opens them.

## Next: Phase 2

GPU decode in `CalcViewData` from `GetSOGRecordData` / `GetSOGPaletteData` plus the tables and constants:
port `shaders/SOGDecode.metal` to `.usf`, drop the legacy buffers for SOG assets, and add 16-bit sort keys
on mobile.

## Reproduce

```bash
tools/phase0/run_phase0.sh "/Volumes/External SSD/Unreal Projects/SHUTourDemo/SourceData/scene_building_nosky.ply"   # Phase 0 data
tests/run_tests.sh
NANOGS_SOG_TESTDATA="$PWD/data" UnrealEditor SHUTourDemo.uproject -ExecCmds="Automation RunTests NanoGS.SOG" \
    -TestExit="Automation Test Queue Empty" -unattended -nullrhi -NoP4
```
