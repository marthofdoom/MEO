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
// Self-exclusion: never read our own output — the source tree must be what
// the load order looks like WITHOUT the patch, or re-runs would compound.
var dropped = resolved.RemoveAll(r =>
    r.Name.Equals("MEO - Patch.esp", StringComparison.OrdinalIgnoreCase));
if (dropped > 0) Console.WriteLine("excluded installed MEO - Patch.esp from the read");
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
    "tree-effects" => Commands.DumpTreeEffects(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "AVEnchanting"),
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

    // Every perk in a skill tree + its NNAM rank chains: what the effects
    // actually do (entry points / abilities) and which perks it requires.
    // Evidence for the keep/replace classifier — never hardcode (prime directive).
    public static int DumpTreeEffects(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var av = lo.PriorityOrder.ActorValueInformation().WinningOverrides()
            .FirstOrDefault(a => a.EditorID == edid);
        if (av is null) return Fail($"{edid} not found");
        foreach (var n in av.PerkTree)
        {
            if (!n.Perk.TryResolve(cache, out var perk)) continue;
            for (var p = perk; p is not null;
                 p = p.NextPerk.TryResolve(cache, out var np) ? np : null)
            {
                // Re-resolve to the WINNING override (NNAM may point at origin).
                if (!cache.TryResolve<IPerkGetter>(p.FormKey, out var w)) break;
                Console.WriteLine($"node {n.Index}: {w.EditorID} '{w.Name}'");
                foreach (var c in w.Conditions)
                    if (c is IConditionFloatGetter cf && cf.Data is IHasPerkConditionDataGetter hp)
                        Console.WriteLine($"    requires: {(hp.Perk.Link.TryResolve(cache, out var rp) ? rp.EditorID : hp.Perk.Link.FormKey.ToString())}");
                foreach (var eff in w.Effects)
                {
                    var kind = eff switch
                    {
                        IPerkEntryPointModifyValueGetter mv => $"entry {mv.EntryPoint} {mv.Modification} {mv.Value}",
                        IPerkEntryPointModifyValuesGetter mvs => $"entry {mvs.EntryPoint}",
                        IPerkEntryPointAddActivateChoiceGetter ac => $"entry {ac.EntryPoint} activate-choice",
                        IPerkEntryPointSelectSpellGetter ss => $"entry {ss.EntryPoint} spell={(ss.Spell.TryResolve(cache, out var sp) ? sp.EditorID : "?")}",
                        IPerkEntryPointSelectTextGetter st => $"entry {st.EntryPoint} text",
                        IPerkEntryPointModifyActorValueGetter mav => $"entry {mav.EntryPoint} av={mav.ActorValue}",
                        IPerkAbilityEffectGetter ab => $"ability {(ab.Ability.TryResolve(cache, out var asp) ? asp.EditorID : "?")}",
                        IPerkQuestEffectGetter q => "quest-stage",
                        _ => eff.GetType().Name,
                    };
                    Console.WriteLine($"    {kind}");
                }
                if (p.NextPerk.IsNull) break;
            }
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

        // Classify every source node by what its perk (and NNAM rank chain)
        // actually DOES — never by name (prime directive). Enchanting-craft
        // entry points mark the system MEO replaces; anything else (staff/
        // wand/charge perks riding in the tree) is kept and rewired.
        static bool IsCraftEntry(APerkEntryPointEffect.EntryType ep)
        {
            var s = ep.ToString();
            return s == "ModEnchantmentPower" || s.Contains("SoulGem");
        }
        IEnumerable<IPerkGetter> RankChain(IPerkGetter first)
        {
            for (var p = first; p is not null;
                 p = !p.NextPerk.IsNull && p.NextPerk.TryResolve(cache, out var np) ? np : null)
            {
                yield return p;
            }
        }
        bool IsCraftPerk(IPerkGetter perk) => RankChain(perk).Any(p =>
            p.Effects.OfType<IAPerkEntryPointEffectGetter>().Any(e => IsCraftEntry(e.EntryPoint)));

        var removedIdx = new HashSet<uint>();
        var removedPerks = new HashSet<FormKey>();
        var keptPerkHeads = new List<IPerkGetter>();
        foreach (var n in av.PerkTree)
        {
            if ((n.Index ?? 0) == 0) continue;
            if (!n.Perk.TryResolve(cache, out var perk) || IsCraftPerk(perk))
            {
                removedIdx.Add(n.Index ?? 0);
                if (perk is not null)
                    foreach (var p in RankChain(perk)) removedPerks.Add(p.FormKey);
            }
            else
            {
                keptPerkHeads.Add(perk);
            }
        }

        var root = over.PerkTree.FirstOrDefault(n => (n.Index ?? 0) == 0)
                   ?? throw new InvalidOperationException("no root node in source tree");
        foreach (var n in over.PerkTree.Where(n => removedIdx.Contains(n.Index ?? 0)).ToList())
            over.PerkTree.Remove(n);
        foreach (var n in over.PerkTree)
            for (int i = n.ConnectionLineToIndices.Count - 1; i >= 0; i--)
                if (removedIdx.Contains(n.ConnectionLineToIndices[i]))
                    n.ConnectionLineToIndices.RemoveAt(i);

        // Kept nodes that lost every parent hang off the root again.
        var referenced = over.PerkTree.Where(n => (n.Index ?? 0) != 0)
            .SelectMany(n => n.ConnectionLineToIndices).ToHashSet();
        var orphans = over.PerkTree.Select(n => n.Index ?? 0)
            .Where(i => i != 0 && !referenced.Contains(i)).ToList();
        root.ConnectionLineToIndices.Clear();
        foreach (var o in orphans) root.ConnectionLineToIndices.Add(o);

        // Kept perks whose HasPerk prerequisite points at a removed perk get a
        // record override with just that dangling condition dropped.
        foreach (var head in keptPerkHeads)
        {
            foreach (var p in RankChain(head))
            {
                var dangling = p.Conditions.Where(c =>
                    c is IConditionFloatGetter cf &&
                    cf.Data is IHasPerkConditionDataGetter hp &&
                    removedPerks.Contains(hp.Perk.Link.FormKey)).ToList();
                if (dangling.Count == 0) continue;
                var po = patch.Perks.GetOrAddAsOverride(p);
                po.Conditions.RemoveAll(c =>
                    c is IConditionFloat cf && cf.Data is IHasPerkConditionDataGetter hp &&
                    removedPerks.Contains(hp.Perk.Link.FormKey));
                Console.WriteLine($"  perk override {p.EditorID}: dropped {dangling.Count} dangling HasPerk condition(s)");
            }
        }

        // MEO nodes get fresh indices and a grid column that doesn't collide
        // with anything kept. Layout (y grows outward from the root):
        //           jeweler   (x , 3)
        //           twinned   (x , 2)
        //   cutter (x-1, 1)   feeder (x+1, 1)
        //           attune1-5 (x , 0)
        var occupied = over.PerkTree.Where(n => (n.Index ?? 0) != 0)
            .Select(n => (n.PerkGridX ?? 0, n.PerkGridY ?? 0)).ToHashSet();
        uint xBase = 2;
        (uint, uint)[] Cells(uint x) => [(x, 0u), (x - 1, 1u), (x + 1, 1u), (x, 2u), (x, 3u)];
        while (Cells(xBase).Any(c => occupied.Contains(c))) xBase++;
        uint nextIdx = over.PerkTree.Max(n => n.Index ?? 0) + 1;

        ActorValuePerkNode Node(uint idx, FormKey perk, uint gx, uint gy, params uint[] conns)
        {
            var n = new ActorValuePerkNode
            {
                Index = idx,
                PerkGridX = gx,
                PerkGridY = gy,
                HorizontalPosition = 0f,
                VerticalPosition = 0f,
                FNAM = new byte[] { 1, 0, 0, 0 },  // uint32 1, as on every vanilla perk node
            };
            n.AssociatedSkill.SetTo(av.FormKey);
            n.Perk.SetTo(perk);
            foreach (var c in conns) n.ConnectionLineToIndices.Add(c);
            return n;
        }

        uint iAtt = nextIdx, iCut = nextIdx + 1, iFeed = nextIdx + 2,
             iTwin = nextIdx + 3, iJwl = nextIdx + 4;
        over.PerkTree.Add(Node(iAtt, attune1, xBase, 0, iCut, iFeed));
        over.PerkTree.Add(Node(iCut, gemCutter, xBase - 1, 1));
        over.PerkTree.Add(Node(iFeed, soulFeeder, xBase + 1, 1, iTwin));
        over.PerkTree.Add(Node(iTwin, twinned, xBase, 2, iJwl));
        over.PerkTree.Add(Node(iJwl, jeweler, xBase, 3));
        root.ConnectionLineToIndices.Add(iAtt);

        Console.WriteLine($"tree: {removedIdx.Count} craft node(s) replaced, " +
                          $"{over.PerkTree.Count - 6} kept, MEO at x={xBase}, " +
                          $"{orphans.Count} kept orphan(s) reparented to root");
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
