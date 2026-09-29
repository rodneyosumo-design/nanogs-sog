# Runs INSIDE the editor (tools/phase2/uepy.py editor_setup.py). Imports scene_nosky.sog (+ Nanite) and the SOG
# round-trip PLY (+ Nanite, legacy NanoGS path) into /Game/NanoGS_SOGTest_Temp and places three actors at the
# origin: NanoGS_A_Legacy (the shipping /Game/Gaussian/scene_nosky), NanoGS_B_SOG, NanoGS_C_RoundtripLegacy.
# Delete Content/NanoGS_SOGTest_Temp afterwards; nothing here is meant to be saved or submitted.
import os, time, unreal
DATA = os.environ.get("NANOGS_SOG_TESTDATA", "/Volumes/External SSD/Unreal Projects/nanogs-sog/data")
TEMP = "/Game/NanoGS_SOGTest_Temp"
def imported(src, name):
    path = TEMP + "/" + name
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        task = unreal.AssetImportTask()
        for k, v in (("filename", src), ("destination_path", TEMP), ("destination_name", name),
                     ("automated", True), ("replace_existing", True), ("save", False)):
            task.set_editor_property(k, v)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(path)
    if not asset.is_nanite_enabled():
        asset.build_nanite_cluster_hierarchy()
    return asset
t0 = time.time()
assets = {"NanoGS_A_Legacy": unreal.load_asset("/Game/Gaussian/scene_nosky"),
          "NanoGS_B_SOG": imported(DATA + "/scene_nosky.sog", "scene_nosky_sog"),
          "NanoGS_C_RoundtripLegacy": imported(DATA + "/scene_nosky_roundtrip.ply", "scene_nosky_roundtrip")}
sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for label, asset in assets.items():
    found = [a for a in sub.get_all_level_actors() if a.get_actor_label() == label]
    actor = found[0] if found else sub.spawn_actor_from_class(unreal.GaussianSplatActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    actor.set_actor_label(label)
    actor.get_editor_property("gaussian_splat_component").set_splat_asset(asset)
    print("%-26s %s: %d splats, %d clusters, %.1f MB" % (label, asset.get_path_name(), asset.get_splat_count(),
          asset.get_cluster_count(), asset.get_memory_usage() / 1e6))
print("setup %.1f s" % (time.time() - t0))
