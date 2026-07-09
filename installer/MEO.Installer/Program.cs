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

const string Usage =
    "usage: MEO.Installer                                     (post-install: auto-detect MO2, write patch)\n" +
    "       MEO.Installer <stats|tree|tree-effects|perk|write-patch> <MO2 root> <profile> [arg]";
if (args.Length > 0 && args[0] is "-h" or "--help" or "help")
{
    Console.WriteLine(Usage);
    return 0;
}
if (args.Length is > 0 and < 3)
{
    Console.Error.WriteLine(Usage);
    return 1;
}

string cmd, mo2Root, profile;
// Trailing options: --sub <esp> replaces the same-named plugin's file (test a
// regenerated plugin in place), --add <esp> appends at the end of the order.
var subs = new List<string>();
var adds = new List<string>();
var positional = new List<string>();
var installMode = args.Length == 0;
if (installMode)
{
    // Post-install mode: the exe ships inside the mod folder (<MO2>/mods/MEO),
    // so the MO2 root is above us. Detect it, pick the profile, and write the
    // patch next to the exe.
    var exeDir = Path.GetDirectoryName(Environment.ProcessPath)
                 ?? Directory.GetCurrentDirectory();
    var root = Mo2LoadOrder.FindRootAbove(exeDir);
    if (root is null)
    {
        if (Console.IsInputRedirected)
        {
            Console.Error.WriteLine("no ModOrganizer.ini found above the exe and no terminal to ask on.");
            Console.Error.WriteLine(Usage);
            return 1;
        }
        Console.Write("MO2 folder not auto-detected. Path to it (the folder holding ModOrganizer.ini): ");
        root = Console.ReadLine()?.Trim().Trim('"');
        if (string.IsNullOrWhiteSpace(root) ||
            !File.Exists(Path.Combine(root, "ModOrganizer.ini")))
        {
            Console.Error.WriteLine("that folder has no ModOrganizer.ini — aborting.");
            return Pause(1, installMode);
        }
    }
    Console.WriteLine($"MO2 root: {root}");
    var picked = Mo2LoadOrder.PickProfile(root);
    if (picked is null) return Pause(Commands.Fail("no profiles found under " + root), installMode);
    (mo2Root, profile, cmd) = (root, picked, "write-patch");
    var outDir = exeDir.StartsWith(Path.Combine(root, "mods"), StringComparison.OrdinalIgnoreCase)
        ? exeDir
        : Path.Combine(root, "mods", "MEO");
    Directory.CreateDirectory(outDir);
    positional.Add(Path.Combine(outDir, "MEO - Patch.esp"));
}
else
{
    (cmd, mo2Root, profile) = (args[0], args[1], args[2]);
    for (int i = 3; i < args.Length; i++)
    {
        if (args[i] == "--sub") subs.Add(args[++i]);
        else if (args[i] == "--add") adds.Add(args[++i]);
        else positional.Add(args[i]);
    }
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

int rc;
try
{
    rc = cmd switch
    {
        "stats" => Commands.Stats(loadOrder),
        "tree" => Commands.DumpTree(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "AVEnchanting"),
        "tree-effects" => Commands.DumpTreeEffects(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "AVEnchanting"),
        "perk" => Commands.DumpPerk(loadOrder, cache, positional[0]),
        "write-patch" => Commands.WritePatch(loadOrder, cache, positional[0]),
        "strip-report" => Commands.StripReport(loadOrder, cache,
            positional.ElementAtOrDefault(0) ?? "data/gem_catalog.json",
            positional.ElementAtOrDefault(1)),
        _ => Commands.Fail($"unknown command {cmd}"),
    };
}
catch (Exception ex)
{
    Console.Error.WriteLine(ex.ToString());
    rc = 1;
}
if (installMode && rc == 0)
    Console.WriteLine("\nDone. In MO2's plugin list (right pane), tick 'MEO - Patch.esp' and keep it at the end.");
return Pause(rc, installMode);

// Double-clicked console windows vanish on exit; hold them open in install mode.
static int Pause(int rc, bool installMode)
{
    if (installMode && !Console.IsInputRedirected)
    {
        Console.Write("\nPress Enter to close...");
        Console.ReadLine();
    }
    return rc;
}

static class Commands
{
    sealed class UncoveredAgg
    {
        public IMagicEffectGetter? M;
        public int Items;
        public int Solo;
        public HashSet<FormKey> Enchs = [];
        public Dictionary<string, int> Partners = [];
        public List<string> Examples = [];
    }

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
                    var nc = (eff as IAPerkEffectGetter)?.Conditions.Count ?? -1;
                    Console.WriteLine($"    {kind} [conds={nc}]");
                    if (eff is IAPerkEffectGetter pe)
                        foreach (var pc in pe.Conditions)
                            foreach (var c in pc.Conditions)
                            {
                                var d = c.Data;
                                var args = string.Join(" ", new[]
                                {
                                    (d as IHasKeywordConditionDataGetter)?.Keyword.Link.TryResolve(cache, out var kw) == true ? kw.EditorID : null,
                                    (d as IEPMagic_SpellHasKeywordConditionDataGetter)?.Keyword.Link.TryResolve(cache, out var kw2) == true ? kw2.EditorID : null,
                                    (d as IWornHasKeywordConditionDataGetter)?.Keyword.Link.TryResolve(cache, out var kw3) == true ? kw3.EditorID : null,
                                    d is IGetIsObjectTypeConditionDataGetter or IEPMagic_SpellHasSkillConditionDataGetter
                                        ? string.Join(",", d.GetType().GetProperties()
                                            .Where(pr => pr.PropertyType.IsEnum || pr.PropertyType == typeof(int) || pr.PropertyType == typeof(uint) || pr.PropertyType == typeof(float))
                                            .Select(pr => $"{pr.Name}={pr.GetValue(d)}"))
                                        : null,
                                }.Where(x => x is not null));
                                var cmp = c is IConditionFloatGetter cff ? $" {cff.CompareOperator} {cff.ComparisonValue}" : "";
                                Console.WriteLine($"        [tab{pc.RunOnTabIndex}] {d.Function} {args}{cmp}");
                            }
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
        // v2: also claim perks whose runtime entries only fire for
        // FormType=Enchantment magic — they empower the enchantment system
        // itself (e.g. Special Feats' Arcane Artificery), which MEO now owns
        // via Gem Attunement; leaving them would double-scale gem output.
        static bool RequiresEnchantmentObject(IAPerkEffectGetter e) =>
            e.Conditions.Any(pc => pc.Conditions.Any(c =>
                c is IConditionFloatGetter cf &&
                cf.CompareOperator == CompareOperator.EqualTo && cf.ComparisonValue == 1 &&
                cf.Data is IGetIsObjectTypeConditionDataGetter g &&
                g.GetType().GetProperty("FormType")?.GetValue(g)?.ToString() == "Enchantment"));
        bool IsCraftPerk(IPerkGetter perk) => RankChain(perk).Any(p =>
            p.Effects.OfType<IAPerkEntryPointEffectGetter>().Any(e => IsCraftEntry(e.EntryPoint)) ||
            p.Effects.OfType<IAPerkEffectGetter>().Any(RequiresEnchantmentObject));

        var removedIdx = new HashSet<uint>();
        var removedPerks = new HashSet<FormKey>();
        var keptPerkHeads = new List<IPerkGetter>();
        var keepCandidates = new List<(uint Idx, IPerkGetter Perk)>();
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
                keepCandidates.Add((n.Index ?? 0, perk));
            }
        }

        // Interactive curation (Marth 2026-07-09): the classifier decides what
        // is MEO's domain; the human decides which surviving perks are worth
        // keeping. Decisions persist next to the patch so re-runs don't re-ask
        // — delete or edit the .choices.json to change your mind.
        var choicesPath = Path.ChangeExtension(outPath, ".choices.json");
        var choices = File.Exists(choicesPath)
            ? System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, bool>>(
                  File.ReadAllText(choicesPath)) ?? []
            : [];
        var interactive = !Console.IsInputRedirected ||
                          Environment.GetEnvironmentVariable("MEO_ASSUME_TTY") == "1";
        foreach (var (idx, perk) in keepCandidates)
        {
            var key = perk.FormKey.ToString();
            if (!choices.TryGetValue(key, out var keep))
            {
                var ranks = RankChain(perk).ToList();
                Console.WriteLine($"\nnon-enchanting perk in the tree: '{perk.Name}'" +
                                  $" ({ranks.Count} rank(s)) [{perk.FormKey.ModKey}]");
                foreach (var r in ranks)
                    if (r.Description?.String is { Length: > 0 } d)
                        Console.WriteLine($"  desc: {d}");
                foreach (var e in perk.Effects.OfType<IAPerkEntryPointEffectGetter>())
                    Console.WriteLine($"  effect: {e.EntryPoint}");
                foreach (var e in perk.Effects.OfType<IPerkAbilityEffectGetter>())
                    Console.WriteLine($"  ability: {(e.Ability.TryResolve(cache, out var sp) ? sp.Name?.String ?? sp.EditorID : "?")}");
                if (interactive)
                {
                    Console.Write("  keep it in the new tree? [Y/n]: ");
                    var a = Console.ReadLine()?.Trim().ToLowerInvariant();
                    keep = a is not ("n" or "no");
                    choices[key] = keep;
                }
                else
                {
                    // Fallback, not a decision: don't record it, so a later
                    // interactive run still asks.
                    keep = true;
                    Console.WriteLine("  (non-interactive: kept, not recorded)");
                }
            }
            Console.WriteLine($"  -> {(keep ? "KEEP" : "REMOVE")} '{perk.Name}'");
            if (keep)
            {
                keptPerkHeads.Add(perk);
            }
            else
            {
                removedIdx.Add(idx);
                foreach (var p in RankChain(perk)) removedPerks.Add(p.FormKey);
            }
        }
        if (choices.Count > 0)
            File.WriteAllText(choicesPath, System.Text.Json.JsonSerializer.Serialize(
                choices, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));

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

    // A magic effect's mechanical identity, independent of which plugin last
    // touched it: what it does (archetype), to what (AVs), and whether it
    // hurts. Two MGEFs with the same signature are the same gameplay effect,
    // which is how "does a gem family cover this enchantment" is decided
    // without ever matching names or FormIDs (the prime directive).
    static string Sig(IMagicEffectGetter m) =>
        $"{m.Archetype.Type}|{m.Archetype.ActorValue}|" +
        $"{(int)(m.Flags & (MagicEffect.Flag.Hostile | MagicEffect.Flag.Detrimental))}|" +
        $"{m.ResistValue}|{m.SecondActorValue}";

    // Read-only census for the loot strip: classifies every winning ENCH by
    // the ruled policy (single-effect family-covered generics and tiered
    // 2-effect generic lines strip; named packages / multi-effect artifacts /
    // blacklist keep) and counts what that means item- and LVLI-wise.
    public static int StripReport(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache,
        string catalogPath, string? dumpPath = null)
    {
        if (!File.Exists(catalogPath)) return Fail($"catalog not found: {catalogPath}");
        var catalog = System.Text.Json.JsonDocument.Parse(File.ReadAllText(catalogPath));
        var covered = new HashSet<string>();
        int unresolved = 0;
        foreach (var fam in catalog.RootElement.EnumerateObject())
            foreach (var r in fam.Value.GetProperty("mgef_refs").EnumerateArray())
            {
                var fk = new FormKey(
                    ModKey.FromNameAndExtension(r.GetProperty("plugin").GetString()!),
                    Convert.ToUInt32(r.GetProperty("fid").GetString()!, 16));
                if (cache.TryResolve<IMagicEffectGetter>(fk, out var m)) covered.Add(Sig(m));
                else unresolved++;
            }
        Console.WriteLine($"catalog: {covered.Count} covered effect signature(s)" +
                          (unresolved > 0 ? $", {unresolved} ref(s) not in this list" : ""));

        // ENCH coverage: resolved effects + whether every one is mirrored by a
        // gem family.
        var enchInfo = new Dictionary<FormKey, (List<(string? Sig, IMagicEffectGetter? M)> Fx, bool Covered)>();
        foreach (var e in lo.PriorityOrder.ObjectEffect().WinningOverrides())
        {
            var fx = e.Effects
                .Select(x => x.BaseEffect.TryResolve(cache, out var m)
                    ? (Sig(m!), (IMagicEffectGetter?)m) : (null, null))
                .ToList();
            enchInfo[e.FormKey] =
                (fx, fx.Count > 0 && fx.All(t => t.Item1 is not null && covered.Contains(t.Item1)));
        }
        Console.WriteLine($"winning ENCH records: {enchInfo.Count} " +
                          $"(fully covered: {enchInfo.Values.Count(v => v.Covered)})");

        // The strip decision is per ITEM, and "generic loot line" is a record
        // shape, not a name list: list-generated enchanted variants are
        // template records whose display name extends their unenchanted
        // base's name ("Iron Sword of Embers" <- template "Iron Sword").
        // Distinctively named gear ("Lunar Iron Sword...", backpacks, thane
        // rewards) fails the prefix test and keeps, with no blacklist needed.
        static bool GenericNamed(string? name, string? baseName) =>
            name is { Length: > 0 } && baseName is { Length: > 0 } &&
            name.Length > baseName.Length &&
            name.StartsWith(baseName, StringComparison.OrdinalIgnoreCase);

        // Fallback for lists that rebuild loot without template links (Requiem
        // replaces vanilla enchanted variants with untemplated REQ_ records):
        // "<unenchanted item's name> of <suffix>" is the loot generator's
        // naming shape, tested against the list's own unenchanted item names.
        var plainNames = new HashSet<string>(StringComparer.Ordinal);
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
            if (w.ObjectEffect.IsNull && w.Name?.String is { Length: > 0 } n) plainNames.Add(n);
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
            if (a.ObjectEffect.IsNull && a.Name?.String is { Length: > 0 } n) plainNames.Add(n);

        bool GenericShaped(string? name)
        {
            if (name is null) return false;
            for (int p = name.IndexOf(" of ", StringComparison.Ordinal); p > 0;
                 p = name.IndexOf(" of ", p + 1, StringComparison.Ordinal))
                if (plainNames.Contains(name[..p])) return true;
            return false;
        }

        string Classify(FormKey ench, bool generic)
        {
            if (!generic) return "keep-named";
            var info = enchInfo.GetValueOrDefault(ench);
            // Casting implements and framework-entangled lines (battlestaffs
            // wired into combat-mod scripts, summon staves): these effects
            // cannot exist inside a gem, so their items stay untouched.
            if (info.Fx is not null && info.Fx.Any(t => t.M?.Archetype.Type.ToString()
                    is "Script" or "SummonCreature" or "SpawnHazard" or "Light" or "Cloak"))
                return "keep-scripted";
            var (n, cov) = (info.Fx?.Count ?? 0, info.Covered);
            return n switch
            {
                1 when cov => "strip-1fx",
                2 when cov => "strip-2fx-recipe",      // gems follow the recipe (riders)
                3 when cov => "strip-3fx-recipe",      // e.g. chaos: tri-element recipe
                2 => "strip-2fx-uncovered",            // generic line, partly outside the catalog
                1 => "keep-generic-uncovered",         // a family MEO has no gem for
                _ => "keep-generic-multifx",
            };
        }

        // Tier chains template on each other ("of the Inferno" -> "of
        // Scorching" -> ... -> base), so walk to the chain's root — the
        // unenchanted item whose name the generics extend.
        var raw = new List<(FormKey Key, FormKey Ench, string? Name, string? BaseName, ModKey Mod, string? Edid)>();
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
        {
            if (w.ObjectEffect.IsNull) continue;
            var root = w;
            for (int i = 0; i < 10 && !root.Template.IsNull; i++)
                if (root.Template.TryResolve(cache, out var t)) root = t; else break;
            var baseName = ReferenceEquals(root, w) ? null : root.Name?.String;
            raw.Add((w.FormKey, w.ObjectEffect.FormKey, w.Name?.String, baseName,
                     w.FormKey.ModKey, w.EditorID));
        }
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
        {
            if (a.ObjectEffect.IsNull) continue;
            var root = a;
            for (int i = 0; i < 10 && !root.TemplateArmor.IsNull; i++)
                if (root.TemplateArmor.TryResolve(cache, out var t)) root = t; else break;
            var baseName = ReferenceEquals(root, a) ? null : root.Name?.String;
            raw.Add((a.FormKey, a.ObjectEffect.FormKey, a.Name?.String, baseName,
                     a.FormKey.ModKey, a.EditorID));
        }

        // Corroboration for the name-shape path: loot generics share their
        // ENCH across many item records; an artifact's enchant is bespoke.
        // "Spear of Bitter Mercy" is name-shaped like a generic but its
        // enchant exists nowhere else -> keep.
        var enchUse = raw.GroupBy(r => r.Ench).ToDictionary(g => g.Key, g => g.Count());
        var items = new List<(FormKey Key, string Cls, string? Name, ModKey Mod, string? Edid)>();
        foreach (var r in raw)
        {
            var generic = GenericNamed(r.Name, r.BaseName) ||
                          (GenericShaped(r.Name) && enchUse[r.Ench] >= 3);
            items.Add((r.Key, Classify(r.Ench, generic), r.Name, r.Mod, r.Edid));
        }
        // Untemplated twins: NPC-hand records (Dremora fire blades etc.) share
        // a stripped generic's display name but not its template shape. They
        // reach players as kill loot, so surface them for a ruling instead of
        // hiding them inside keep-named.
        var stripNames = items.Where(i => i.Cls.StartsWith("strip") && i.Name is not null)
            .Select(i => i.Name!).ToHashSet(StringComparer.Ordinal);
        items = items.Select(i =>
            i.Cls == "keep-named" && i.Name is not null && stripNames.Contains(i.Name)
                ? i with { Cls = "review-npc-twin" } : i).ToList();

        var stripItems = items.Where(i => i.Cls.StartsWith("strip")).Select(i => i.Key).ToHashSet();
        var counts = items.GroupBy(i => i.Cls).ToDictionary(g => g.Key, g => g.Count());
        Console.WriteLine("enchanted item classes: " + string.Join("  ",
            counts.OrderByDescending(kv => kv.Value).Select(kv => $"{kv.Key}={kv.Value}")));
        foreach (var g in items.GroupBy(i => i.Cls).OrderBy(g => g.Key))
            Console.WriteLine($"  {g.Key}: {string.Join("; ",
                g.Take(6).Select(i => $"{i.Name} [{i.Mod}]"))}");
        if (dumpPath is not null)
        {
            File.WriteAllLines(dumpPath, items.OrderBy(i => i.Cls).ThenBy(i => i.Name)
                .Select(i => $"{i.Cls}\t{i.Name}\t{i.Edid}\t{i.Key}"));
            Console.WriteLine($"full census -> {dumpPath}");
        }

        // What the catalog is missing: every effect signature that appears on
        // generic-shaped loot but no gem family mirrors. Pairing tells the
        // story — an uncovered effect that only rides alongside covered
        // primaries is a rider recipe; one that stands alone is a family
        // candidate.
        var clsByItem = items.ToDictionary(i => i.Key, i => i.Cls);
        var agg = new Dictionary<string, UncoveredAgg>();
        foreach (var r in raw)
        {
            if (clsByItem[r.Key] == "keep-named") continue;
            if (!enchInfo.TryGetValue(r.Ench, out var info) || info.Fx is null || info.Covered)
                continue;
            foreach (var (sig, m) in info.Fx)
            {
                if (sig is null || m is null || covered.Contains(sig)) continue;
                var a = agg.TryGetValue(sig, out var got) ? got : agg[sig] = new() { M = m };
                a.Items++;
                a.Enchs.Add(r.Ench);
                if (a.Examples.Count < 3 && r.Name is not null && !a.Examples.Contains(r.Name))
                    a.Examples.Add(r.Name);
                var partners = info.Fx
                    .Where(o => o.Sig is not null && o.Sig != sig && o.M is not null)
                    .Select(o => (o.M!.Name?.String ?? o.M.EditorID ?? "?") +
                                 (covered.Contains(o.Sig!) ? "" : "*"))
                    .ToList();
                if (partners.Count == 0) a.Solo++;
                foreach (var p in partners)
                    a.Partners[p] = a.Partners.GetValueOrDefault(p) + 1;
            }
        }
        Console.WriteLine($"\nuncovered effects on generic loot: {agg.Count} signature(s) " +
                          "(* = partner also uncovered)");
        foreach (var (_, a) in agg.OrderByDescending(kv => kv.Value.Items).Take(25))
        {
            var m = a.M!;
            Console.WriteLine(
                $"  [{a.Items} item(s), {a.Enchs.Count} ench(s), solo x{a.Solo}] " +
                $"'{m.Name?.String ?? m.EditorID}' ({m.EditorID} [{m.FormKey.ModKey}]) " +
                $"arch={m.Archetype.Type} av={m.Archetype.ActorValue} " +
                $"resist={m.ResistValue} av2={m.SecondActorValue}");
            if (m.Description?.String is { Length: > 0 } d)
                Console.WriteLine($"      \"{(d.Length > 110 ? d[..110] + "…" : d)}\"");
            if (a.Partners.Count > 0)
                Console.WriteLine("      pairs with: " + string.Join(", ",
                    a.Partners.OrderByDescending(p => p.Value).Take(4)
                        .Select(p => $"{p.Key} x{p.Value}")));
            Console.WriteLine($"      e.g. {string.Join("; ", a.Examples)}");
        }

        int lvliTouched = 0, entriesTotal = 0;
        var top = new List<(string, int)>();
        foreach (var l in lo.PriorityOrder.LeveledItem().WinningOverrides())
        {
            var hits = l.Entries?.Count(en =>
                en.Data is not null && stripItems.Contains(en.Data.Reference.FormKey)) ?? 0;
            if (hits == 0) continue;
            lvliTouched++;
            entriesTotal += hits;
            top.Add(($"{l.EditorID} [{l.FormKey.ModKey}]", hits));
        }
        Console.WriteLine($"LVLI impact: {entriesTotal} entr(ies) across {lvliTouched} leveled list(s)");
        foreach (var (name, n) in top.OrderByDescending(t => t.Item2).Take(15))
            Console.WriteLine($"  {n,4}  {name}");
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

    // Walk up from the exe (shipped in <MO2>/mods/MEO) to the portable MO2
    // root — the folder holding both ModOrganizer.ini and profiles/.
    public static string? FindRootAbove(string start)
    {
        for (var d = new DirectoryInfo(start); d is not null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "ModOrganizer.ini")) &&
                Directory.Exists(Path.Combine(d.FullName, "profiles")))
                return d.FullName;
        return null;
    }

    // ModOrganizer.ini's selected_profile is the default; only ask when the
    // instance has more than one profile.
    public static string? PickProfile(string root)
    {
        var profiles = Directory.EnumerateDirectories(Path.Combine(root, "profiles"))
            .Where(p => File.Exists(Path.Combine(p, "plugins.txt")))
            .Select(Path.GetFileName)
            .OfType<string>()
            .Order()
            .ToList();
        if (profiles.Count == 0) return null;

        var line = File.ReadLines(Path.Combine(root, "ModOrganizer.ini"))
            .FirstOrDefault(l => l.StartsWith("selected_profile=", StringComparison.Ordinal));
        var sel = line?["selected_profile=".Length..].Trim();
        if (sel is not null && sel.StartsWith("@ByteArray(") && sel.EndsWith(')'))
            sel = sel["@ByteArray(".Length..^1];
        var def = profiles.FirstOrDefault(p => p.Equals(sel, StringComparison.OrdinalIgnoreCase))
                  ?? profiles[0];
        if (profiles.Count == 1 || Console.IsInputRedirected)
        {
            Console.WriteLine($"profile: {def}");
            return def;
        }

        Console.WriteLine("profiles:");
        for (int i = 0; i < profiles.Count; i++)
            Console.WriteLine($"  {i + 1}. {profiles[i]}");
        Console.Write($"which profile? [Enter = {def}]: ");
        var a = Console.ReadLine()?.Trim();
        if (string.IsNullOrEmpty(a)) return def;
        if (int.TryParse(a, out var n) && n >= 1 && n <= profiles.Count) return profiles[n - 1];
        return profiles.FirstOrDefault(p => p.Equals(a, StringComparison.OrdinalIgnoreCase)) ?? def;
    }

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
