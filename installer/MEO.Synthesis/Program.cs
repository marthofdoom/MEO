// MEO.Synthesis — the marth's Enchanting Overhaul patcher as a Synthesis patch.
//
// Does exactly what the standalone installer does, into Synthesis's own output
// mod: (a) rewrites the enchanting perk tree (AVEnchanting) to MEO's gem perks,
// and (b) derives + writes meo_calibration.json for this load order. Both steps
// call the SHARED Commands.* code (MEO.Installer/Commands.cs), so the output is
// byte-identical to the standalone installer for the same load order.

using Mutagen.Bethesda;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Synthesis;

return await SynthesisPipeline.Instance
    .AddPatch<ISkyrimMod, ISkyrimModGetter>(RunPatch)
    .SetTypicalOpen(GameRelease.SkyrimSE, "MEO - Patch.esp")
    .Run(args);

static void RunPatch(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
{
    // Match the standalone installer's ESL flag: MEO - Patch.esp is a pure
    // override plugin, so it costs no load-order slot.
    state.PatchMod.IsSmallMaster = true;

    // (a) Perk-tree edit into Synthesis's output mod. Non-interactive KEEP-ALL
    // (no choicesPath) — the standalone tool's curated foreign-perk file has no
    // meaning here; Synthesis users curate via their own load order.
    var rc = Commands.ApplyPatch(state.LoadOrder, state.LinkCache, state.PatchMod);
    if (rc != 0)
        throw new Exception($"MEO ApplyPatch failed (rc={rc}) — see log above");

    // (b) Calibration. The catalog ships next to this assembly.
    var asmDir = Path.GetDirectoryName(
        System.Reflection.Assembly.GetExecutingAssembly().Location)
        ?? AppContext.BaseDirectory;
    var catalogPath = Path.Combine(asmDir, "gem_catalog.json");

    // Write into the game data tree; under MO2 this lands in the overwrite folder.
    var meoDir = Path.Combine(state.DataFolderPath, "SKSE", "Plugins", "MEO");
    Directory.CreateDirectory(meoDir);
    var calOutPath = Path.Combine(meoDir, "meo_calibration.json");

    var crc = Commands.WriteCalibration(state.LoadOrder, state.LinkCache, catalogPath, calOutPath);
    if (crc != 0)
        throw new Exception($"MEO WriteCalibration failed (rc={crc}) — see log above");
}
