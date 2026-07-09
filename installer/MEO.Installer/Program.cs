// MEO.Installer — install-time patcher for Marth's Enchanting Overhaul.
//
// Resolves an MO2 profile's load order on Linux (no VFS running), reads it
// with Mutagen, and generates "MEO - Patch.esp": an override of the
// enchanting actor-value record that replaces whatever perk tree the load
// order shipped with MEO's 9 gem perks.
//
// Usage:
//   MEO.Installer stats <MO2 root> <profile>
//   MEO.Installer tree  <MO2 root> <profile> <AVIF EditorID>
//   MEO.Installer perk  <MO2 root> <profile> <PERK EditorID>
//   MEO.Installer write-patch <MO2 root> <profile> <output esp path>

using System.Diagnostics;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

if (args.Length < 3)
{
    Console.Error.WriteLine("usage: MEO.Installer <stats|tree|perk|write-patch> <MO2 root> <profile> [arg]");
    return 1;
}

var (cmd, mo2Root, profile) = (args[0], args[1], args[2]);
// Trailing options: --sub <esp> replaces the same-named plugin's file (test a
// regenerated plugin in place), --add <esp> appends at the end of the order.
var subs = new List<string>();
var adds = new List<string>();
var positional = new List<string>();
for (int i = 3; i < args.Length; i++)
{
    if (args[i] == "--sub") subs.Add(args[++i]);
    else if (args[i] == "--add") adds.Add(args[++i]);
    else positional.Add(args[i]);
}

var resolved = Mo2LoadOrder.Resolve(mo2Root, profile, out var missing);
foreach (var s in subs)
{
    var fname = Path.GetFileName(s);
    var idx = resolved.FindIndex(r => r.Name.Equals(fname, StringComparison.OrdinalIgnoreCase));
    if (idx < 0) return Commands.Fail($"--sub {fname}: not in load order");
    resolved[idx] = (resolved[idx].Name, Path.GetFullPath(s));
    Console.WriteLine($"substituted {fname} -> {s}");
}
foreach (var a in adds)
{
    resolved.Add((Path.GetFileName(a), Path.GetFullPath(a)));
    Console.WriteLine($"appended {Path.GetFileName(a)}");
}
Console.WriteLine($"{resolved.Count} plugins resolved, {missing.Count} missing");
if (missing.Count > 0)
    Console.WriteLine("  missing: " + string.Join(", ", missing.Take(10)));

var sw = Stopwatch.StartNew();
var listings = new List<IModListingGetter<ISkyrimModGetter>>();
foreach (var (name, path) in resolved)
{
    var mod = SkyrimMod.CreateFromBinaryOverlay(
        new ModPath(ModKey.FromNameAndExtension(name), path),
        SkyrimRelease.SkyrimSE);
    listings.Add(new ModListing<ISkyrimModGetter>(mod, enabled: true));
}
var loadOrder = new LoadOrder<IModListingGetter<ISkyrimModGetter>>(listings);
var cache = loadOrder.ToImmutableLinkCache();
Console.WriteLine($"load order read in {sw.Elapsed.TotalSeconds:F1}s");

return cmd switch
{
    "stats" => Commands.Stats(loadOrder),
    "tree" => Commands.DumpTree(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "AVEnchanting"),
    "perk" => Commands.DumpPerk(loadOrder, cache, positional[0]),
    "write-patch" => Commands.WritePatch(loadOrder, cache, positional[0]),
    _ => Commands.Fail($"unknown command {cmd}"),
};

static class Commands
{
    public static int Fail(string msg)
    {
        Console.Error.WriteLine(msg);
        return 1;
    }

    public static int Stats(LoadOrder<IModListingGetter<ISkyrimModGetter>> lo)
    {
        long ench = 0, perk = 0, lvli = 0, avif = 0;
        foreach (var l in lo.ListedOrder)
        {
            var m = l.Mod!;
            ench += m.ObjectEffects.Count;
            perk += m.Perks.Count;
            lvli += m.LeveledItems.Count;
            avif += m.ActorValueInformation.Count;
        }
        Console.WriteLine($"totals: ENCH={ench} PERK={perk} LVLI={lvli} AVIF={avif}");
        return 0;
    }

    public static int DumpTree(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var av = lo.PriorityOrder.ActorValueInformation().WinningOverrides()
            .FirstOrDefault(a => a.EditorID == edid);
        if (av is null) return Fail($"{edid} not found");
        Console.WriteLine($"{edid} winning override: {av.FormKey}");
        Console.WriteLine($"perk tree nodes: {av.PerkTree.Count}");
        foreach (var n in av.PerkTree)
        {
            var perkId = n.Perk.TryResolve(cache, out var p)
                ? $"{p.EditorID} [{p.FormKey}]" : $"<{n.Perk.FormKey}>";
            Console.WriteLine(
                $"  idx={n.Index,3} fnam={(n.FNAM is {} fb ? Convert.ToHexString(fb.ToArray()) : "null")} grid=({n.PerkGridX},{n.PerkGridY})" +
                $" pos=({n.HorizontalPosition:F2},{n.VerticalPosition:F2})" +
                $" skill={n.AssociatedSkill}" +
                $" -> [{string.Join(",", n.ConnectionLineToIndices)}]  {perkId}");
        }
        return 0;
    }

    public static int DumpPerk(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var perk = lo.PriorityOrder.Perk().WinningOverrides()
            .FirstOrDefault(p => p.EditorID == edid);
        if (perk is null) return Fail($"{edid} not found");
        Console.WriteLine($"{perk.EditorID} [{perk.FormKey}] '{perk.Name}'");
        Console.WriteLine($"  playable={perk.Playable} trait={perk.Trait} level={perk.Level}" +
                          $" numRanks={perk.NumRanks} hidden={perk.Hidden}");
        var next = perk.NextPerk.TryResolve(cache, out var np)
            ? $"{np.EditorID} [{np.FormKey}]" : perk.NextPerk.FormKeyNullable?.ToString() ?? "null";
        Console.WriteLine($"  nextPerk: {next}");
        foreach (var c in perk.Conditions)
        {
            if (c is IConditionFloatGetter cf)
            {
                var detail = cf.Data is IGetBaseActorValueConditionDataGetter av
                    ? $" av={av.ActorValue}" : $" data={cf.Data}";
                Console.WriteLine($"  cond: {cf.Data.Function} {cf.CompareOperator} {cf.ComparisonValue}" +
                                  $" flags={cf.Flags} runOn={cf.Data.RunOnType}{detail}");
            }
            else
                Console.WriteLine($"  cond: {c}");
        }
        return 0;
    }

    public static int WritePatch(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string outPath)
    {
        var av = lo.PriorityOrder.ActorValueInformation().WinningOverrides()
            .FirstOrDefault(a => a.EditorID == "AVEnchanting");
        if (av is null) return Fail("AVEnchanting not found");

        var meo = ModKey.FromNameAndExtension("MEO.esp");
        if (!lo.ListedOrder.Any(l => l.ModKey == meo))
            return Fail("MEO.esp not in load order — install MEO first");

        // MEO.esp-local perk FormIDs (DESIGN §6; FROZEN).
        FormKey Perk(uint local) => new(meo, local);
        var attune1 = Perk(0x810);   // ranked 1..5 via NNAM chain inside MEO.esp
        var gemCutter = Perk(0x815);
        var soulFeeder = Perk(0x816);
        var twinned = Perk(0x817);
        var jeweler = Perk(0x818);

        var patch = new SkyrimMod(ModKey.FromNameAndExtension("MEO - Patch.esp"),
                                  SkyrimRelease.SkyrimSE)
        {
            // Pure-override plugin: ESL-flag it so it costs no load-order slot.
            IsSmallMaster = true,
        };
        var over = patch.ActorValueInformation.GetOrAddAsOverride(av);

        // Preserve the vanilla root node (index 0, null perk, odd grid coords the
        // UI expects) and rewire it to MEO's tree. Everything else is replaced.
        var root = over.PerkTree.FirstOrDefault(n => n.Index == 0)
                   ?? throw new InvalidOperationException("no root node in source tree");
        over.PerkTree.Clear();
        root.ConnectionLineToIndices.Clear();
        root.ConnectionLineToIndices.Add(1);
        over.PerkTree.Add(root);

        ActorValuePerkNode Node(uint idx, FormKey perk, uint gx, uint gy,
                                float hpos, float vpos, params uint[] conns)
        {
            var n = new ActorValuePerkNode
            {
                Index = idx,
                PerkGridX = gx,
                PerkGridY = gy,
                HorizontalPosition = hpos,
                VerticalPosition = vpos,
                FNAM = new byte[] { 1, 0, 0, 0 },  // uint32 1, as on every vanilla perk node
            };
            n.AssociatedSkill.SetTo(av.FormKey);
            n.Perk.SetTo(perk);
            foreach (var c in conns) n.ConnectionLineToIndices.Add(c);
            return n;
        }

        // Layout (gridY grows outward from the root):
        //            [5] Master Jeweler        (2,3)
        //            [4] Twinned Fitting       (2,2)
        //   [2] Gem Cutter (1,1)   [3] Soul Feeder (3,1)
        //            [1] Gem Attunement 1-5    (2,0)
        over.PerkTree.Add(Node(1, attune1, 2, 0, 0f, 0f, 2, 3));
        over.PerkTree.Add(Node(2, gemCutter, 1, 1, 0f, 0f));
        over.PerkTree.Add(Node(3, soulFeeder, 3, 1, 0f, 0f, 4));
        over.PerkTree.Add(Node(4, twinned, 2, 2, 0f, 0f, 5));
        over.PerkTree.Add(Node(5, jeweler, 2, 3, 0f, 0f));

        patch.WriteToBinary(outPath);
        Console.WriteLine($"wrote {outPath}");
        Console.WriteLine($"masters: {string.Join(", ", patch.ModHeader.MasterReferences.Select(m => m.Master))}");
        return 0;
    }
}

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
