// MEO.Installer — install-time patcher for Marth's Enchanting Overhaul.
//
// Stage 3c scaffold: prove we can resolve an MO2 profile's load order on
// Linux (no VFS running) and read every enabled plugin with Mutagen.
//
// Usage: MEO.Installer <MO2 root> <profile>

using System.Diagnostics;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

if (args.Length < 2)
{
    Console.Error.WriteLine("usage: MEO.Installer <MO2 root> <profile>");
    return 1;
}

var (mo2Root, profile) = (args[0], args[1]);
var resolved = Mo2LoadOrder.Resolve(mo2Root, profile, out var missing);
Console.WriteLine($"{resolved.Count} plugins resolved, {missing.Count} missing");
if (missing.Count > 0)
    Console.WriteLine("  missing: " + string.Join(", ", missing.Take(10)));

var sw = Stopwatch.StartNew();
var listings = new List<IModListingGetter<ISkyrimModGetter>>();
long enchTotal = 0, perkTotal = 0, lvliTotal = 0, avifTotal = 0;
foreach (var (name, path) in resolved)
{
    var mod = SkyrimMod.CreateFromBinaryOverlay(
        new ModPath(ModKey.FromNameAndExtension(name), path),
        SkyrimRelease.SkyrimSE);
    enchTotal += mod.ObjectEffects.Count;
    perkTotal += mod.Perks.Count;
    lvliTotal += mod.LeveledItems.Count;
    avifTotal += mod.ActorValueInformation.Count;
    listings.Add(new ModListing<ISkyrimModGetter>(mod, enabled: true));
}
var loadOrder = new LoadOrder<IModListingGetter<ISkyrimModGetter>>(listings);
sw.Stop();
Console.WriteLine($"read {listings.Count} plugins in {sw.Elapsed.TotalSeconds:F1}s");
Console.WriteLine($"totals: ENCH={enchTotal} PERK={perkTotal} LVLI={lvliTotal} AVIF={avifTotal}");

// Winning-override proof: dump the Enchanting AV's perk tree as the game
// actually sees it (whichever plugin last touched AVEnchanting wins).
sw.Restart();
var cache = loadOrder.ToImmutableLinkCache();
var enchAv = loadOrder.PriorityOrder.ActorValueInformation().WinningOverrides()
    .FirstOrDefault(av => av.EditorID == "AVEnchanting");
if (enchAv is null)
{
    Console.Error.WriteLine("AVEnchanting not found");
    return 1;
}
Console.WriteLine($"\nAVEnchanting winning override: {enchAv.FormKey}");
Console.WriteLine($"perk tree nodes: {enchAv.PerkTree.Count}");
foreach (var node in enchAv.PerkTree)
{
    var perkId = node.Perk.TryResolve(cache, out var perk)
        ? $"{perk.EditorID} [{perk.FormKey}]"
        : $"<unresolved {node.Perk.FormKey}>";
    var conns = string.Join(",", node.ConnectionLineToIndices);
    Console.WriteLine($"  idx={node.Index,3} ({node.PerkGridX},{node.PerkGridY})" +
                      $" -> [{conns}]  {perkId}");
}
sw.Stop();
Console.WriteLine($"link-cache resolve pass: {sw.Elapsed.TotalSeconds:F1}s");
return 0;

/// <summary>
/// Resolves an MO2 profile to concrete plugin paths without the VFS:
/// modlist.txt is highest-priority-first (first hit wins), with the
/// Stock Game data folder as final fallback. Case-insensitive on Linux.
/// </summary>
static class Mo2LoadOrder
{
    static readonly string[] BaseMasters =
        ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];

    public static List<(string Name, string Path)> Resolve(
        string mo2Root, string profile, out List<string> missing)
    {
        var prof = Path.Combine(mo2Root, "profiles", profile);
        var plugins = File.ReadLines(Path.Combine(prof, "plugins.txt"))
            .Where(l => l.StartsWith('*'))
            .Select(l => l[1..].Trim())
            .ToList();
        var mods = File.ReadLines(Path.Combine(prof, "modlist.txt"))
            .Where(l => l.StartsWith('+') && !l.TrimEnd().EndsWith("_separator"))
            .Select(l => l[1..].Trim());

        var search = mods.Select(m => Path.Combine(mo2Root, "mods", m))
            .Append(Path.Combine(mo2Root, "Stock Game", "Data"));
        var dirMaps = new List<(string Dir, Dictionary<string, string> Files)>();
        foreach (var d in search)
        {
            if (!Directory.Exists(d)) continue;
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var f in Directory.EnumerateFiles(d))
                map.TryAdd(Path.GetFileName(f), Path.GetFileName(f));
            dirMaps.Add((d, map));
        }

        var order = BaseMasters.Concat(plugins);
        var outList = new List<(string, string)>();
        missing = [];
        foreach (var p in order)
        {
            var hit = dirMaps.FirstOrDefault(dm => dm.Files.ContainsKey(p));
            if (hit.Dir is not null)
                outList.Add((p, Path.Combine(hit.Dir, hit.Files[p])));
            else
                missing.Add(p);
        }
        return outList;
    }
}
