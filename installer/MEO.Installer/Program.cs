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
    "usage: MEO.Installer                                     (post-install: auto-detect MO2, write patch + calibration)\n" +
    "       MEO.Installer <stats|tree|tree-effects|perk|ench|item|write-patch|strip-report|write-calibration> <MO2-or-game root> <profile-or-plugins.txt|auto> [args]";
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

string cmd = "", mo2Root = "", profile = "";
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
    // Vanilla compatibility: the mod unzipped into <game>/Data with no mod
    // manager — the game folder above the exe is the whole load order.
    var gameRoot = root is null ? Mo2LoadOrder.FindGameRootAbove(exeDir) : null;
    if (root is null && gameRoot is not null)
    {
        Console.WriteLine($"game root (no MO2): {gameRoot}");
        (mo2Root, profile, cmd) = (gameRoot, "auto", "write-patch");
        var vOut = Path.GetDirectoryName(Environment.ProcessPath) ?? gameRoot;
        positional.Add(Path.Combine(vOut, "MEO - Patch.esp"));
        Console.WriteLine("NOTE: after the run, enable 'MEO - Patch.esp' in the in-game " +
                          "CREATIONS/MODS menu or your plugins.txt.");
    }
    else if (root is null)
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
    if (root is not null)  // MO2 flow; the game-root flow set everything above
    {
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
    r.Name.Equals("MEO - Patch.esp", StringComparison.OrdinalIgnoreCase) ||
    r.Name.Equals("MEO - Strip.esp", StringComparison.OrdinalIgnoreCase));
if (dropped > 0) Console.WriteLine($"excluded {dropped} installed MEO output esp(s) from the read");
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
        "ench" => Commands.DumpEnch(loadOrder, cache, positional[0]),
        "item" => Commands.DumpItem(loadOrder, cache, positional[0]),
        "npc" => Commands.DumpNpc(loadOrder, cache, positional[0]),
        "spell" => Commands.DumpSpell(loadOrder, cache, positional[0]),
        "write-calibration" => Commands.WriteCalibration(loadOrder, cache,
            positional.ElementAtOrDefault(0) ?? "data/gem_catalog.json",
            positional.ElementAtOrDefault(1) ?? "meo_calibration.json"),
        _ => Commands.Fail($"unknown command {cmd}"),
    };
}
catch (Exception ex)
{
    Console.Error.WriteLine(ex.ToString());
    rc = 1;
}
if (installMode && rc == 0)
{
    // Rider calibration: derive each gem family's recipe from this list's own
    // generic loot lines. The catalog ships inside the mod next to the exe.
    var modDir = Path.GetDirectoryName(positional[0])!;
    var meoDir = Path.Combine(modDir, "SKSE", "Plugins", "MEO");
    var catPath = Path.Combine(meoDir, "gem_catalog.json");
    if (File.Exists(catPath))
    {
        Console.WriteLine();
        var calOut = Path.Combine(meoDir, "meo_calibration.json");
        Console.WriteLine($"writing calibration -> {calOut}");
        try
        {
            rc = Commands.WriteCalibration(loadOrder, cache, catPath, calOut);
        }
        catch (Exception ex)
        {
            // m32e (Steam Deck field loss): this ran OUTSIDE the command
            // try/catch — an exception under a double-clicked Wine console
            // vanished with the window, leaving no calibration and NO
            // conversions in game, silently. Never again.
            Console.Error.WriteLine("CALIBRATION FAILED — conversions will not work until this is fixed:");
            Console.Error.WriteLine(ex.ToString());
            rc = 1;
        }
    }
    else
        Console.WriteLine($"\nnote: {catPath} missing — rider calibration skipped, " +
                          "compiled defaults stay in force");
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
        foreach (var eff in perk.Effects)
        {
            var tn = eff.GetType().Name;
            var ep = eff.GetType().GetProperty("EntryPoint")?.GetValue(eff);
            var fn = eff.GetType().GetProperty("Function")?.GetValue(eff);
            Console.WriteLine($"  effect: {tn}" +
                              (ep != null ? $" entryPoint={ep}" : "") +
                              (fn != null ? $" function={fn}" : ""));
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
        var pyrestone = Perk(0x819);   // m34 affinities + Facet Insight
        var froststone = Perk(0x81A);
        var stormstone = Perk(0x81B);
        var facet = Perk(0x81C);

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
        (uint, uint)[] Cells(uint x) => [(x, 0u), (x - 1, 1u), (x + 1, 1u), (x, 2u), (x, 3u),
                                         (x - 2, 1u), (x + 2, 1u), (x - 2, 2u), (x + 2, 2u)];
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
             iTwin = nextIdx + 3, iJwl = nextIdx + 4,
             iPyr = nextIdx + 5, iFro = nextIdx + 6, iSto = nextIdx + 7, iFac = nextIdx + 8;
        // Attunement is the hub; affinities + Facet Insight branch off it
        // alongside cutter/feeder (m34, DESIGN §6 reqs 30-50).
        over.PerkTree.Add(Node(iAtt, attune1, xBase, 0, iCut, iFeed, iPyr, iFro, iFac));
        over.PerkTree.Add(Node(iCut, gemCutter, xBase - 1, 1));
        over.PerkTree.Add(Node(iFeed, soulFeeder, xBase + 1, 1, iTwin));
        over.PerkTree.Add(Node(iTwin, twinned, xBase, 2, iJwl));
        over.PerkTree.Add(Node(iJwl, jeweler, xBase, 3));
        over.PerkTree.Add(Node(iPyr, pyrestone, xBase - 2, 1, iSto));   // pyre -> storm chain
        over.PerkTree.Add(Node(iFro, froststone, xBase + 2, 1));
        over.PerkTree.Add(Node(iSto, stormstone, xBase - 2, 2));
        over.PerkTree.Add(Node(iFac, facet, xBase + 2, 2));
        root.ConnectionLineToIndices.Add(iAtt);

        Console.WriteLine($"tree: {removedIdx.Count} craft node(s) replaced, " +
                          $"{over.PerkTree.Count - 10} kept, MEO at x={xBase}, " +
                          $"{orphans.Count} kept orphan(s) reparented to root");
        patch.WriteToBinary(outPath);
        Console.WriteLine($"wrote {outPath}");
        Console.WriteLine($"masters: {string.Join(", ", patch.ModHeader.MasterReferences.Select(m => m.Master))}");
        return 0;
    }

    public static int DumpEnch(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var hits = lo.PriorityOrder.ObjectEffect().WinningOverrides()
            .Where(e => (e.EditorID?.Contains(edid, StringComparison.OrdinalIgnoreCase) ?? false) ||
                        (e.Name?.String?.Contains(edid, StringComparison.OrdinalIgnoreCase) ?? false))
            .Take(4).ToList();
        if (hits.Count == 0) return Fail($"no winning ENCH matching '{edid}'");
        foreach (var e in hits)
        {
            Console.WriteLine($"{e.EditorID} '{e.Name?.String}' [{e.FormKey}]");
            foreach (var x in e.Effects)
            {
                if (!x.BaseEffect.TryResolve(cache, out var m))
                { Console.WriteLine("  (unresolved effect)"); continue; }
                Console.WriteLine($"  {m.EditorID} '{m.Name?.String}' [{m.FormKey}] " +
                    $"arch={m.Archetype.Type} av={m.Archetype.ActorValue} resist={m.ResistValue} " +
                    $"av2={m.SecondActorValue} mag={x.Data?.Magnitude} dur={x.Data?.Duration} " +
                    $"conds={x.Conditions.Count} mgefConds={m.Conditions.Count}");
                if (m.Description?.String is { Length: > 0 } md)
                    Console.WriteLine($"      desc: \"{(md.Length > 140 ? md[..140] + "…" : md)}\"");
                if (m.Keywords is { Count: > 0 })
                    Console.WriteLine("      kw: " + string.Join(", ", m.Keywords.Select(k =>
                        k.TryResolve(cache, out var kg) ? kg.EditorID ?? k.FormKey.ToString()
                                                        : k.FormKey.ToString())));
            }
        }
        return 0;
    }

    // Record inspector: winning WEAP/ARMO by name/editorid — prints the
    // template chain (the names GenericNamed compares) and the ENCH link,
    // i.e. exactly the inputs the census classifier sees for the item.
    public static int DumpItem(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string needle)
    {
        bool Match(string? s) => s?.Contains(needle, StringComparison.OrdinalIgnoreCase) ?? false;
        int shown = 0;
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
        {
            if (shown >= 8) break;
            if (!Match(w.Name?.String) && !Match(w.EditorID)) continue;
            shown++;
            Console.WriteLine($"WEAP {w.EditorID} '{w.Name?.String}' [{w.FormKey}] " +
                $"ench={(w.ObjectEffect.IsNull ? "none" : w.ObjectEffect.FormKey.ToString())}");
            var root = w;
            for (int i = 0; i < 10 && !root.Template.IsNull; i++)
            {
                if (!root.Template.TryResolve(cache, out var t)) break;
                root = t;
                Console.WriteLine($"  template -> {root.EditorID} '{root.Name?.String}' [{root.FormKey}]");
            }
        }
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
        {
            if (shown >= 16) break;
            if (!Match(a.Name?.String) && !Match(a.EditorID)) continue;
            shown++;
            Console.WriteLine($"ARMO {a.EditorID} '{a.Name?.String}' [{a.FormKey}] " +
                $"ench={(a.ObjectEffect.IsNull ? "none" : a.ObjectEffect.FormKey.ToString())}");
            var root = a;
            for (int i = 0; i < 10 && !root.TemplateArmor.IsNull; i++)
            {
                if (!root.TemplateArmor.TryResolve(cache, out var t)) break;
                root = t;
                Console.WriteLine($"  template -> {root.EditorID} '{root.Name?.String}' [{root.FormKey}]");
            }
        }
        return shown > 0 ? 0 : Fail($"no winning WEAP/ARMO matching '{needle}'");
    }

    public static int DumpSpell(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string needle)
    {
        var hits = lo.PriorityOrder.Spell().WinningOverrides()
            .Where(s => (s.EditorID?.Contains(needle, StringComparison.OrdinalIgnoreCase) ?? false) ||
                        (s.Name?.String?.Contains(needle, StringComparison.OrdinalIgnoreCase) ?? false))
            .Take(5).ToList();
        if (hits.Count == 0) return Fail($"no winning SPEL matching '{needle}'");
        foreach (var s in hits)
        {
            Console.WriteLine($"{s.EditorID} '{s.Name?.String}' [{s.FormKey}] type={s.Type}");
            foreach (var x in s.Effects)
            {
                if (!x.BaseEffect.TryResolve(cache, out var m))
                { Console.WriteLine("  (unresolved effect)"); continue; }
                Console.WriteLine($"  {m.EditorID} '{m.Name?.String}' [{m.FormKey}] " +
                    $"arch={m.Archetype.Type} av={m.Archetype.ActorValue} resist={m.ResistValue} " +
                    $"mag={x.Data?.Magnitude} dur={x.Data?.Duration} " +
                    $"conds={x.Conditions.Count} mgefConds={m.Conditions.Count}");
                if (m.Description?.String is { Length: > 0 } d)
                    Console.WriteLine($"      \"{(d.Length > 140 ? d[..140] + "…" : d)}\"");
                void ShowCond(IConditionGetter c, string tag)
                {
                    var cf = c as IConditionFloatGetter;
                    var fn = c.Data.GetType().Name.Replace("ConditionDataBinaryOverlay", "");
                    var ps = string.Join(",", c.Data.GetType().GetProperties()
                        .Where(p => p.Name is "FirstParameter" or "SecondParameter" or
                                    "ActorValue" or "Keyword" or "Value")
                        .Select(p =>
                        {
                            var v = p.GetValue(c.Data);
                            var linkObj = v?.GetType().GetProperty("Link")?.GetValue(v);
                            if (linkObj?.GetType().GetProperty("FormKey")?.GetValue(linkObj)
                                is FormKey fk)
                            {
                                if (cache.TryResolve<IKeywordGetter>(fk, out var kw))
                                    return $"{p.Name}={kw.EditorID}";
                                if (cache.TryResolve<IMagicEffectGetter>(fk, out var mg))
                                    return $"{p.Name}={mg.EditorID}";
                                if (cache.TryResolve<ISpellGetter>(fk, out var sp))
                                    return $"{p.Name}={sp.EditorID}";
                                return $"{p.Name}={fk}";
                            }
                            return $"{p.Name}={v}";
                        }));
                    Console.WriteLine($"      {tag}: {fn}({ps}) {cf?.CompareOperator} {cf?.ComparisonValue} " +
                                      $"runOn={c.Data.RunOnType}");
                }
                foreach (var c in x.Conditions) ShowCond(c, "cond");
                foreach (var c in m.Conditions) ShowCond(c, "mgefCond");
            }
        }
        return 0;
    }

    public static int DumpNpc(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string needle)
    {
        var hits = lo.PriorityOrder.Npc().WinningOverrides()
            .Where(n => (n.EditorID?.Contains(needle, StringComparison.OrdinalIgnoreCase) ?? false) ||
                        (n.Name?.String?.Contains(needle, StringComparison.OrdinalIgnoreCase) ?? false))
            .Take(5).ToList();
        if (hits.Count == 0) return Fail($"no winning NPC matching '{needle}'");
        foreach (var n in hits)
        {
            Console.WriteLine($"{n.EditorID} '{n.Name?.String}' [{n.FormKey}]");
            foreach (var s in n.ActorEffect)
                if (s.TryResolve(cache, out var sp))
                    Console.WriteLine($"  spell/ability: {sp.EditorID} '{(sp as ISpellGetter)?.Name?.String}'");
            foreach (var p in n.Perks ?? [])
                if (p.Perk.TryResolve(cache, out var pk))
                    Console.WriteLine($"  perk: {pk.EditorID} '{pk.Name?.String}'");
            foreach (var it in n.Items ?? [])
                if (it.Item.Item.TryResolve(cache, out var item))
                    Console.WriteLine($"  item: {item.EditorID} x{it.Item.Count} ({item.GetType().Name.Replace("BinaryOverlay", "")})");
            foreach (var k in n.Keywords ?? [])
                if (k.TryResolve(cache, out var kw) && (kw.EditorID?.Contains("Automaton") == true ||
                    kw.EditorID?.Contains("Dwarven") == true))
                    Console.WriteLine($"  keyword: {kw.EditorID}");
        }
        return 0;
    }

    // Derive each gem family's rider recipe from the list's own generic loot
    // lines (the prime directive: read what the list does, never assume).
    // For every strip-classified ENCH, the family is the catalog entry whose
    // full effect-signature multiset the enchant contains (chaos's 3-sig set
    // outranks fire's 1-sig set on a tri-element enchant). Whatever effects
    // remain after the family's own are consumed — plus the family's 2nd/3rd
    // components — are that recipe's riders: magnitude ratio vs the primary,
    // plus duration. The dominant recipe (by item count) wins.
    public static int WriteCalibration(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache,
        string catalogPath, string outPath)
    {
        if (BuildCensus(lo, cache, catalogPath) is not { } data) return 1;

        var clsByItem = data.Items.ToDictionary(i => i.Key, i => i.Cls);
        var enchWeight = new Dictionary<FormKey, int>();
        foreach (var r in data.Raw)
            if (clsByItem[r.Key].StartsWith("strip") || clsByItem[r.Key] == "keep-generic-multifx")
                enchWeight[r.Ench] = enchWeight.GetValueOrDefault(r.Ench) + 1;

        // Human-curated rulings (Marth's tuning loop): effects HE has ruled
        // perk-domain — Requiem grants them through perks, they are not
        // native to equipment. They (a) never count as conversion loss and
        // (b) NEVER ride on gems (2026-07-10: lockpick durability rode @60
        // the moment absolute riders landed — wrong domain). Marked for a
        // deeper perk-vs-equipment classification pass later.
        var ruledWaived = new HashSet<FormKey>();
        var rulingsPath = outPath.Replace(".json", ".rulings.json");
        if (File.Exists(rulingsPath))
        {
            using var doc2 = System.Text.Json.JsonDocument.Parse(File.ReadAllText(rulingsPath));
            if (doc2.RootElement.TryGetProperty("waivedEffects", out var arr))
                foreach (var e in arr.EnumerateArray())
                {
                    var fk = e.GetProperty("formKey").GetString()!.Split(':');
                    ruledWaived.Add(new FormKey(ModKey.FromFileName(fk[1]),
                                                Convert.ToUInt32(fk[0], 16)));
                }
            Console.WriteLine($"rulings: {ruledWaived.Count} waived effect(s) from {rulingsPath}");
        }

        var fams = new Dictionary<string, Dictionary<string, RecipeAgg>>();
        var enchFamily = new Dictionary<FormKey, string>();
        var enchLeftover =
            new Dictionary<FormKey, List<(string? Sig, IMagicEffectGetter? M, float Mag, int Dur, int Conds)>>();
        var noteWeight = new Dictionary<string, int>();
        void Note(string msg, int weight) =>
            noteWeight[msg] = noteWeight.GetValueOrDefault(msg) + weight;
        foreach (var (ench, weight) in enchWeight)
        {
            var fx = data.EnchInfo[ench].Fx
                .Where(t => t.Sig is not null && t.M is not null).ToList();
            if (fx.Count == 0) continue;

            string? best = null;
            int bestN = 0;
            bool bestFirst = false;
            foreach (var (famKey, refs) in data.FamilyRefs)
            {
                var pool = fx.Select(t => t.Sig!).ToList();
                if (!refs.All(r => pool.Remove(r.Sig))) continue;
                var first = refs[0].Sig == fx[0].Sig;
                if (refs.Count > bestN || (refs.Count == bestN && first && !bestFirst))
                    (best, bestN, bestFirst) = (famKey, refs.Count, first);
            }
            if (Environment.GetEnvironmentVariable("MEO_CAL_DEBUG") == "1")
                Console.WriteLine($"DBG\t{best ?? "NO-FAMILY"}\tw={weight}\t" +
                    string.Join(" | ", fx.Select(t => $"{t.M!.EditorID}={t.Sig}")));
            if (best is null) continue;
            enchFamily[ench] = best;

            // Consume the family's own components in catalog order; the first
            // is the primary the ratios normalize against. Prefer FormKey
            // identity over signature so a sig-colliding companion (turn
            // undead + a heal that reads as the same value-modifier) isn't
            // mistaken for the family's own effect.
            var refs2 = data.FamilyRefs[best];
            var pool2 = new List<(string? Sig, IMagicEffectGetter? M, float Mag, int Dur, int Conds)>(fx);
            var matched = new List<(string? Sig, IMagicEffectGetter? M, float Mag, int Dur, int Conds)>();
            foreach (var (rk, rs) in refs2)
            {
                var idx = pool2.FindIndex(t => t.M!.FormKey == rk);
                if (idx < 0) idx = pool2.FindIndex(t => t.Sig == rs);
                matched.Add(pool2[idx]);
                pool2.RemoveAt(idx);
            }
            enchLeftover[ench] = pool2;  // unmatched companions — the loss audit's input
            var prim = matched[0];
            // m32: a zero-magnitude primary (paralyze, soul trap) can't ratio-
            // normalize — its riders ride ABSOLUTE instead: flat magnitude and
            // duration observed from the recipe itself. This is what unpins
            // the 'of Stunning' line: those ARE the paralysis family's recipe.
            var abs = prim.Mag <= 0;
            // A conditional companion (Requiem's double-shock-vs-Dwemer bonus)
            // only fires under its conditions; gem riders carry none, so
            // copying it would apply the bonus always. Skip and say so.
            var all = matched.Skip(1).Concat(pool2).ToList();
            foreach (var d in all.Where(t => t.M is not null && ruledWaived.Contains(t.M.FormKey)))
                Note($"{best}: ruled perk-domain '{d.M!.Name?.String ?? d.M.EditorID}' never rides", weight);
            var riders = all.Where(t => t.Conds == 0 &&
                                        (t.M is null || !ruledWaived.Contains(t.M.FormKey))).ToList();
            foreach (var d in all.Where(t => t.Conds > 0))
                Note($"{best}: conditional companion '{d.M!.Name?.String ?? d.M.EditorID}' " +
                     $"({d.Conds} cond(s)) not carried", weight);
            var famRec = fams.TryGetValue(best, out var f) ? f : fams[best] = [];
            var key = string.Join("+", riders.Select(t => t.M!.FormKey)) + (abs ? "|abs" : "");
            var rec = famRec.TryGetValue(key, out var got) ? got : famRec[key] = new()
            { Mgefs = riders.Select(t => t.M!).ToList(), Abs = abs };
            rec.Weight += weight;
            rec.Enchs++;
            for (int i = 0; i < riders.Count; i++)
            {
                if (rec.Obs.Count <= i) rec.Obs.Add([]);
                rec.Obs[i].Add((abs ? riders[i].Mag : riders[i].Mag / prim.Mag, riders[i].Dur));
            }
        }

        static float Median(List<float> v)
        {
            var s = v.Order().ToList();
            return s.Count % 2 == 1 ? s[s.Count / 2] : (s[s.Count / 2 - 1] + s[s.Count / 2]) / 2f;
        }

        var outFams = new SortedDictionary<string, object>();
        foreach (var (famKey, recipes) in fams.OrderBy(kv => kv.Key))
        {
            var ranked = recipes.Values.OrderByDescending(r => r.Weight).ToList();
            var dom = ranked[0];
            var alts = ranked.Skip(1).Sum(r => r.Weight);
            var line = $"{famKey}: ";
            if (dom.Mgefs.Count == 0)
            {
                // Explicit empty rider list: the DLL must CLEAR any compiled
                // default for a family this list's recipe keeps plain.
                outFams[famKey] = new Dictionary<string, object>
                {
                    ["riders"] = new List<object>(),
                    ["from"] = $"{dom.Weight} item(s), {dom.Enchs} ench(s)",
                };
                line += $"no riders (plain recipe, {dom.Weight} item(s))";
                if (alts > 0) line += $" — minority recipes on {alts} item(s) ignored";
                Console.WriteLine("  " + line);
                continue;
            }
            if (dom.Mgefs.Count > 4)
                Note($"{famKey}: recipe has {dom.Mgefs.Count} riders, DLL carries 4 — truncated",
                     dom.Weight);
            var riderJson = new List<object>();
            var parts = new List<string>();
            for (int i = 0; i < Math.Min(dom.Mgefs.Count, 4); i++)
            {
                var m = dom.Mgefs[i];
                var val = (float)Math.Round(Median(dom.Obs[i].Select(o => o.Ratio).ToList()), 3);
                var dur = (int)Median(dom.Obs[i].Select(o => (float)o.Dur).ToList());
                if (val <= 0 && dur <= 0) continue;  // m32: a nothing rider (REQ_DEPRECATED_*)
                var rj = new Dictionary<string, object>
                {
                    ["plugin"] = m.FormKey.ModKey.FileName.String,
                    ["fid"] = $"0x{m.FormKey.ID:X6}",
                    ["dur"] = dur,
                    ["mgef"] = m.EditorID ?? m.Name?.String ?? "?",
                };
                rj[dom.Abs ? "mag" : "ratio"] = val;  // m32: absolute recipes
                riderJson.Add(rj);
                parts.Add($"{m.Name?.String ?? m.EditorID} {(dom.Abs ? "@" : "x")}{val}/{dur}s");
            }
            outFams[famKey] = new Dictionary<string, object>
            {
                ["riders"] = riderJson,
                ["from"] = $"{dom.Weight} item(s), {dom.Enchs} ench(s)",
            };
            line += string.Join(" + ", parts) + $"  (from {dom.Weight} item(s))";
            if (alts > 0) line += $" — minority recipes on {alts} item(s)";
            Console.WriteLine("  " + line);
        }
        // Which companion MGEFs each family's gems will actually CARRY —
        // the lossless-conversion test reads from this, not from wishes.
        var adopted = new Dictionary<string, HashSet<FormKey>>();
        foreach (var (famKey, obj) in outFams)
        {
            var set = new HashSet<FormKey>();
            foreach (var rj in (List<object>)((Dictionary<string, object>)obj)["riders"])
            {
                var d = (Dictionary<string, object>)rj;
                set.Add(new FormKey(ModKey.FromFileName((string)d["plugin"]),
                                    Convert.ToUInt32(((string)d["fid"])[2..], 16)));
            }
            adopted[famKey] = set;
        }
        // m28 (Marth + records): lists RANK-TIER kin MGEFs of one signature,
        // and the higher ranks carry protection KEYWORDS the attacking spells
        // check (REQ_ProtectionFromAbsorbSpells on Fortify Magicka Rank II).
        // Map gem levels onto that ladder so leveling EARNS the protections:
        // kin ordered by observed enchant magnitude, spread across levels 1-5.
        var kinBySig = new Dictionary<string, Dictionary<FormKey, (IMagicEffectGetter M, float MinMag)>>();
        // m32c (Marth: 'why does fortify magicka do irresistible fire at
        // level V?'): a Vonos ARTIFACT effect shared sig+name-root and won
        // the top rung. Kin may only come from GENERIC (strip-class) enchants
        // — uniques keep their tricks.
        var genericEnchs = data.Raw.Where(r => clsByItem[r.Key].StartsWith("strip"))
            .Select(r => r.Ench).ToHashSet();
        foreach (var (enchKey, info) in data.EnchInfo)
        {
            if (info.Fx is null || !genericEnchs.Contains(enchKey)) continue;
            foreach (var fx in info.Fx)
            {
                if (fx.Sig is null || fx.M is null || fx.Mag <= 0) continue;
                var d = kinBySig.TryGetValue(fx.Sig, out var got) ? got : kinBySig[fx.Sig] = [];
                d[fx.M.FormKey] = d.TryGetValue(fx.M.FormKey, out var prev)
                    ? (fx.M, Math.Min(prev.MinMag, fx.Mag)) : (fx.M, fx.Mag);
            }
        }
        foreach (var (famKey, obj) in outFams)
        {
            var refs = data.FamilyRefs[famKey];
            if (refs.Count == 0) continue;
            if (!kinBySig.TryGetValue(refs[0].Sig, out var kin) || kin.Count < 2) continue;
            // Kin, not strangers: raw signatures collide across the whole
            // magic ecosystem (spell FX, Godform steps, runes). A ladder
            // rung must ALSO match the primary's cast shape and be a name
            // relative — '<primary name>' or '<primary name> (Rank N)'.
            if (!cache.TryResolve<IMagicEffectGetter>(refs[0].Key, out var prim0)) continue;
            static string Root(string? s)
            {
                if (s is null) return "";
                var p = s.IndexOf('(');
                return (p > 0 ? s[..p] : s).Trim().ToLowerInvariant();
            }
            var primRoot = Root(prim0.Name?.String ?? prim0.EditorID);
            if (primRoot.Length == 0) continue;
            var tiers = kin.Values
                .Where(k => Root(k.M.Name?.String ?? k.M.EditorID) == primRoot &&
                            k.M.CastType == prim0.CastType &&
                            k.M.TargetType == prim0.TargetType)
                .OrderBy(k => k.MinMag).Select(k => k.M)
                .DistinctBy(m => m.FormKey).ToList();
            if (tiers.Count < 2 || tiers.Count > 5) continue;  // no plausible ladder
            var lvl = new List<object>();
            for (int l = 0; l < 5; l++)
            {
                var m = tiers[Math.Min(tiers.Count - 1, l * tiers.Count / 5)];
                lvl.Add(new Dictionary<string, object>
                {
                    ["plugin"] = m.FormKey.ModKey.FileName.String,
                    ["fid"] = $"0x{m.FormKey.ID:X6}",
                    ["mgef"] = m.EditorID ?? "?",
                });
            }
            ((Dictionary<string, object>)obj)["levels"] = lvl;
            Console.WriteLine($"  {famKey}: {tiers.Count}-rank ladder -> levels " +
                string.Join(" | ", tiers.Select(m => m.Name?.String ?? m.EditorID)));
        }
        var notes = noteWeight.OrderByDescending(kv => kv.Value)
            .Select(kv => $"{kv.Key} — {kv.Value} item(s)").ToList();
        foreach (var n in notes) Console.WriteLine("  note: " + n);

        // Loot conversion (Marth's ruling): a covered enchanted generic never
        // leaves the world — at spawn the DLL swaps it for its unenchanted
        // base with the matching family's gem socketed and ACTIVE (level I/II
        // rolled with the same sliders as loose drops). This is the table;
        // recipes the gems can't express yet (duration-anchored) are absent,
        // so those items keep spawning enchanted.
        var honored = new HashSet<string> { "strip-1fx", "strip-2fx-recipe", "strip-3fx-recipe",
                                            "strip-2fx-uncovered", "keep-generic-multifx" };
        var conversions = new List<object>();
        int noFamily = 0, pinned = 0, partial = 0, machineryWaived = 0;
        var pinnedByEnch = new Dictionary<FormKey, string>();
        var waivedFx = new Dictionary<string, int>();
        foreach (var r in data.Raw)
        {
            if (!honored.Contains(clsByItem[r.Key])) continue;
            if (!data.StripBase.TryGetValue(r.Key, out var baseKey)) continue;
            if (!enchFamily.TryGetValue(r.Ench, out var famKey)) { noFamily++; continue; }
            // Lossless gate (Marth's ruling, 2026-07-10): convert ONLY when
            // every UNCOVERED companion is either carried by the family's
            // adopted riders or is hidden framework machinery (HideInUI —
            // Requiem/perk-applied bookkeeping, not loot value). Anything
            // else pins the item: it stays enchanted rather than lose value.
            var lost = new List<string>();
            bool waived = false;
            foreach (var l in enchLeftover.GetValueOrDefault(r.Ench) ?? [])
            {
                if (l.Sig is not null && data.Covered.Contains(l.Sig)) continue;  // family-expressible
                if (l.M is not null &&
                    (adopted.GetValueOrDefault(famKey)?.Contains(l.M.FormKey) ?? false))
                    continue;  // rides
                if (l.M is not null && ruledWaived.Contains(l.M.FormKey))
                { waived = true; continue; }  // Marth ruled it perk-domain
                // Machinery = hidden AND valueless. HideInUI alone is not
                // enough: Requiem hides real procs too (the stagger companion
                // on 'of Stunning' weapons carries mag 20-30 while hidden —
                // 2026-07-10 audit). A zero-magnitude hidden effect is
                // bookkeeping (REQ_DEPRECATED_*), safe to wave through.
                if (l.M is not null && l.Mag <= 0 &&
                    l.M.Flags.HasFlag(MagicEffect.Flag.HideInUI))
                {
                    waived = true;
                    var wk = $"{l.M.EditorID} arch={l.M.Archetype.Type} mag={l.Mag} dur={l.Dur}";
                    waivedFx[wk] = waivedFx.GetValueOrDefault(wk) + 1;
                    continue;  // hidden machinery — audited below
                }
                lost.Add(l.M?.Name?.String ?? l.M?.EditorID ?? l.Sig ?? "?");
            }
            if (lost.Count > 0)
            {
                pinned++;
                pinnedByEnch.TryAdd(r.Ench, string.Join(", ", lost.Distinct()));
                continue;
            }
            if (waived) machineryWaived++;
            if (clsByItem[r.Key] is "strip-2fx-uncovered" or "keep-generic-multifx") partial++;
            conversions.Add(new Dictionary<string, object>
            {
                ["plugin"] = r.Key.ModKey.FileName.String,
                ["fid"] = $"0x{r.Key.ID:X6}",
                ["basePlugin"] = baseKey.ModKey.FileName.String,
                ["baseFid"] = $"0x{baseKey.ID:X6}",
                ["family"] = famKey,
            });
        }
        Console.WriteLine($"conversions: {conversions.Count} enchanted generic(s) -> socketed base " +
                          $"({partial} partial-recipe, {machineryWaived} machinery-waived, " +
                          $"{pinned} PINNED lossy" +
                          (noFamily > 0 ? $", {noFamily} no-family)" : ")"));
        foreach (var (ench, lostFx) in pinnedByEnch.OrderBy(kv => kv.Value))
            Console.WriteLine($"  pinned: ench {ench} would lose [{lostFx}]");
        foreach (var (fx, n) in waivedFx.OrderByDescending(kv => kv.Value))
            Console.WriteLine($"  waived machinery x{n}: {fx}");

        var doc = new Dictionary<string, object>
        {
            ["version"] = 1,
            ["generated"] = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"),
            ["families"] = outFams,
            ["conversions"] = conversions,
            ["notes"] = notes,
        };
        File.WriteAllText(outPath, System.Text.Json.JsonSerializer.Serialize(doc,
            new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"wrote {outPath} ({outFams.Count} matched famil(ies), " +
                          "absent families keep compiled defaults)");
        return 0;
    }

    sealed class RecipeAgg
    {
        public bool Abs;   // m32: riders carry flat magnitude, not a ratio
        public int Weight;
        public int Enchs;
        public List<IMagicEffectGetter> Mgefs = [];
        public List<List<(float Ratio, int Dur)>> Obs = [];
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

    // Shared loot census: what every winning ENCH does (resolved effects with
    // magnitudes), which effects the gem catalog mirrors, and the per-item
    // strip/keep classification. strip-report prints it; write-calibration
    // derives rider recipes from it.
    sealed class CensusData
    {
        public HashSet<string> Covered = [];
        public Dictionary<string, List<(FormKey Key, string Sig)>> FamilyRefs = [];
        public Dictionary<FormKey, (List<(string? Sig, IMagicEffectGetter? M, float Mag, int Dur, int Conds)> Fx, bool Covered, bool CastLike)> EnchInfo = [];
        public List<(FormKey Key, FormKey Ench, string? Name, string? BaseName, FormKey? Root, ModKey Mod, string? Edid)> Raw = [];
        public List<(FormKey Key, string Cls, string? Name, ModKey Mod, string? Edid)> Items = [];
        public Dictionary<FormKey, FormKey> StripBase = [];   // strip item -> unenchanted base
    }

    static CensusData? BuildCensus(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string catalogPath)
    {
        if (!File.Exists(catalogPath))
        {
            Console.Error.WriteLine($"catalog not found: {catalogPath}");
            return null;
        }
        var data = new CensusData();
        var catalog = System.Text.Json.JsonDocument.Parse(File.ReadAllText(catalogPath));
        int unresolved = 0;
        foreach (var fam in catalog.RootElement.EnumerateObject())
        {
            var refs = new List<(FormKey Key, string Sig)>();
            foreach (var r in fam.Value.GetProperty("mgef_refs").EnumerateArray())
            {
                var fk = new FormKey(
                    ModKey.FromNameAndExtension(r.GetProperty("plugin").GetString()!),
                    Convert.ToUInt32(r.GetProperty("fid").GetString()!, 16));
                if (cache.TryResolve<IMagicEffectGetter>(fk, out var m))
                {
                    var s = Sig(m);
                    refs.Add((fk, s));
                    data.Covered.Add(s);
                }
                else unresolved++;
            }
            if (refs.Count > 0) data.FamilyRefs[fam.Name] = refs;
        }
        Console.WriteLine($"catalog: {data.Covered.Count} covered effect signature(s)" +
                          (unresolved > 0 ? $", {unresolved} ref(s) not in this list" : ""));

        foreach (var e in lo.PriorityOrder.ObjectEffect().WinningOverrides())
        {
            var fx = e.Effects
                .Select(x => x.BaseEffect.TryResolve(cache, out var m)
                    ? (Sig(m!), (IMagicEffectGetter?)m, x.Data?.Magnitude ?? 0f,
                       x.Data?.Duration ?? 0, x.Conditions.Count)
                    : (null, null, 0f, 0, 0))
                .ToList();
            // Casting implements, not carried enchantments: staff-type ENCH
            // records and concentration/aimed effects (battlestaff spell
            // lines). Script ARCHETYPES are fine — a gem references the same
            // winning MGEF and the engine runs whatever it does; Requiem's
            // Slow rider is a Script-archetype contact effect.
            var castLike = e.EnchantType.ToString() == "StaffEnchantment" ||
                fx.Any(t => t.Item2 is { } m2 &&
                    (m2.CastType == CastType.Concentration ||
                     m2.TargetType == TargetType.Aimed));
            data.EnchInfo[e.FormKey] =
                (fx, fx.Count > 0 && fx.All(t => t.Item1 is not null && data.Covered.Contains(t.Item1)),
                 castLike);
        }
        Console.WriteLine($"winning ENCH records: {data.EnchInfo.Count} " +
                          $"(fully covered: {data.EnchInfo.Values.Count(v => v.Covered)})");

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
        var plainByName = new Dictionary<string, FormKey>(StringComparer.Ordinal);
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
            if (w.ObjectEffect.IsNull && w.Name?.String is { Length: > 0 } n)
                plainByName.TryAdd(n, w.FormKey);
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
            if (a.ObjectEffect.IsNull && a.Name?.String is { Length: > 0 } n)
                plainByName.TryAdd(n, a.FormKey);

        FormKey? GenericShapedBase(string? name)
        {
            if (name is null) return null;
            for (int p = name.IndexOf(" of ", StringComparison.Ordinal); p > 0;
                 p = name.IndexOf(" of ", p + 1, StringComparison.Ordinal))
                if (plainByName.TryGetValue(name[..p], out var fk)) return fk;
            return null;
        }

        string Classify(FormKey ench, bool generic)
        {
            if (!generic) return "keep-named";
            var info = data.EnchInfo.GetValueOrDefault(ench);
            if (info.CastLike) return "keep-cast";
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
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
        {
            if (w.ObjectEffect.IsNull) continue;
            var root = w;
            for (int i = 0; i < 10 && !root.Template.IsNull; i++)
                if (root.Template.TryResolve(cache, out var t)) root = t; else break;
            var baseName = ReferenceEquals(root, w) ? null : root.Name?.String;
            data.Raw.Add((w.FormKey, w.ObjectEffect.FormKey, w.Name?.String, baseName,
                          ReferenceEquals(root, w) ? null : root.FormKey,
                          w.FormKey.ModKey, w.EditorID));
        }
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
        {
            if (a.ObjectEffect.IsNull) continue;
            var root = a;
            for (int i = 0; i < 10 && !root.TemplateArmor.IsNull; i++)
                if (root.TemplateArmor.TryResolve(cache, out var t)) root = t; else break;
            var baseName = ReferenceEquals(root, a) ? null : root.Name?.String;
            data.Raw.Add((a.FormKey, a.ObjectEffect.FormKey, a.Name?.String, baseName,
                          ReferenceEquals(root, a) ? null : root.FormKey,
                          a.FormKey.ModKey, a.EditorID));
        }

        // Corroboration for the name-shape path: loot generics share their
        // ENCH across many item records; an artifact's enchant is bespoke.
        // "Spear of Bitter Mercy" is name-shaped like a generic but its
        // enchant exists nowhere else -> keep.
        var enchUse = data.Raw.GroupBy(r => r.Ench).ToDictionary(g => g.Key, g => g.Count());
        static bool OfShaped(string? name) =>
            name is { Length: > 4 } && name.Contains(" of ", StringComparison.Ordinal);
        int renamedBase = 0;
        foreach (var r in data.Raw)
        {
            FormKey? baseKey = null;
            if (GenericNamed(r.Name, r.BaseName)) baseKey = r.Root;
            else if (enchUse[r.Ench] >= 3)
            {
                baseKey = GenericShapedBase(r.Name);
                // Renamed-base generics (Marth 2026-07-09, "keep-named leak"):
                // Requiem renames template bases ("Glass Bow of Fire" sits on
                // "Glass Light Bow", "Glass Armor of ..." on "Glass Cuirass"),
                // so both name tests fail even though the ENCH is a shared
                // loot recipe. Of-shape + shared ENCH + a template root is
                // generic — and the root IS the unenchanted conversion base.
                if (baseKey is null && r.Root is not null && OfShaped(r.Name))
                {
                    baseKey = r.Root;
                    renamedBase++;
                }
            }
            var cls = Classify(r.Ench, baseKey is not null);
            // m25: partial generics (family + companions we don't cover) are
            // conversion candidates too — record their base (Marth's Squire
            // cuirass blockade: Fortify Light Armor + 3 armor-pen companions).
            if ((cls.StartsWith("strip") || cls == "keep-generic-multifx") && baseKey is { } bk)
                data.StripBase[r.Key] = bk;
            data.Items.Add((r.Key, cls, r.Name, r.Mod, r.Edid));
        }
        if (renamedBase > 0)
            Console.WriteLine($"census: {renamedBase} renamed-base generic(s) reclassified via template root");
        // Untemplated twins: NPC-hand records (Dremora fire blades etc.) share
        // a stripped generic's display name but not its template shape. They
        // reach players as kill loot, so surface them for a ruling instead of
        // hiding them inside keep-named.
        var stripNames = data.Items.Where(i => i.Cls.StartsWith("strip") && i.Name is not null)
            .Select(i => i.Name!).ToHashSet(StringComparer.Ordinal);
        data.Items = data.Items.Select(i =>
            i.Cls == "keep-named" && i.Name is not null && stripNames.Contains(i.Name)
                ? i with { Cls = "review-npc-twin" } : i).ToList();
        return data;
    }

    // Read-only census for the loot strip: classifies every winning ENCH by
    // the ruled policy (single-effect family-covered generics and tiered
    // 2-effect generic lines strip; named packages / multi-effect artifacts /
    // blacklist keep) and counts what that means item- and LVLI-wise.
    public static int StripReport(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache,
        string catalogPath, string? dumpPath = null)
    {
        if (BuildCensus(lo, cache, catalogPath) is not { } data) return 1;
        var (covered, enchInfo, raw, items) = (data.Covered, data.EnchInfo, data.Raw, data.Items);

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
            foreach (var (sig, m, _, _, _) in info.Fx)
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
                $"resist={m.ResistValue} av2={m.SecondActorValue}" +
                (m.Flags.HasFlag(MagicEffect.Flag.HideInUI) ? "  [HIDDEN machinery]" : ""));
            if (m.Description?.String is { Length: > 0 } d)
                Console.WriteLine($"      \"{(d.Length > 110 ? d[..110] + "…" : d)}\"");
            if (a.Partners.Count > 0)
                Console.WriteLine("      pairs with: " + string.Join(", ",
                    a.Partners.OrderByDescending(p => p.Value).Take(4)
                        .Select(p => $"{p.Key} x{p.Value}")));
            Console.WriteLine($"      e.g. {string.Join("; ", a.Examples)}");
        }

        // Player-learnable coverage (Marth 2026-07-10: the in-game no-family
        // dump is bootstrap-only — the INSTALLER must surface this per list).
        // Any enchant on a disenchantable item is learnable at a pre-MEO
        // table, so its effects can live on in player-made instance enchants
        // ('Glass Helmet' case). Effects here with no gem family are the
        // future no-family conversions — catalog candidates for THIS list.
        var disallowKw = new FormKey(ModKey.FromFileName("Skyrim.esm"), 0x0C27BD);
        var learnable = new HashSet<FormKey>();
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
            if (!w.ObjectEffect.IsNull &&
                !(w.Keywords?.Any(k => k.FormKey == disallowKw) ?? false))
                learnable.Add(w.ObjectEffect.FormKey);
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
            if (!a.ObjectEffect.IsNull &&
                !(a.Keywords?.Any(k => k.FormKey == disallowKw) ?? false))
                learnable.Add(a.ObjectEffect.FormKey);
        var learnMiss = new Dictionary<FormKey, (IMagicEffectGetter M, int Enchs)>();
        foreach (var ek in learnable)
        {
            if (!data.EnchInfo.TryGetValue(ek, out var info) || info.Fx is null) continue;
            foreach (var fx in info.Fx)
                if (fx.Sig is not null && fx.M is not null && !data.Covered.Contains(fx.Sig))
                    learnMiss[fx.M.FormKey] = (fx.M,
                        learnMiss.GetValueOrDefault(fx.M.FormKey).Enchs + 1);
        }
        var visMiss = learnMiss.Values.Where(v => !v.M.Flags.HasFlag(MagicEffect.Flag.HideInUI))
            .OrderByDescending(v => v.Enchs).ToList();
        Console.WriteLine($"\nplayer-learnable enchant effects with NO gem family: " +
            $"{learnMiss.Count} ({visMiss.Count} visible = catalog candidates; rest hidden machinery)");
        foreach (var lm in visMiss)
            Console.WriteLine($"  [{lm.Enchs} learnable ench(s)] '{lm.M.Name?.String ?? lm.M.EditorID}' " +
                $"({lm.M.EditorID} [{lm.M.FormKey}]) arch={lm.M.Archetype.Type} " +
                $"av={lm.M.Archetype.ActorValue}");

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

    // ── Vanilla / non-MO2 compatibility ──────────────────────────────
    // A plain game install has no profiles: the load order is Data/ plus the
    // game's own plugins.txt (%LOCALAPPDATA%/Skyrim Special Edition). The
    // <root> argument may be the game folder (holds Data/Skyrim.esm) and the
    // <profile> argument a plugins.txt path, or "auto" to use the game's own.
    public static bool IsGameRoot(string root) =>
        File.Exists(Path.Combine(root, "Data", "Skyrim.esm"));

    public static string? FindGameRootAbove(string start)
    {
        for (var d = new DirectoryInfo(start); d is not null; d = d.Parent)
            if (IsGameRoot(d.FullName))
                return d.FullName;
        return null;
    }

    public static List<(string Name, string Path)> ResolveGame(
        string gameRoot, string pluginsArg, out List<string> missing)
    {
        var dataDir = Path.Combine(gameRoot, "Data");
        string? pluginsPath = null;
        if (!string.IsNullOrEmpty(pluginsArg) && pluginsArg != "auto" &&
            File.Exists(pluginsArg))
            pluginsPath = pluginsArg;
        else
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var cand = Path.Combine(local, "Skyrim Special Edition", "plugins.txt");
            if (File.Exists(cand)) pluginsPath = cand;
        }
        var plugins = pluginsPath is null
            ? new List<string>()
            : File.ReadLines(pluginsPath)
                .Where(l => l.StartsWith('*'))
                .Select(l => l[1..].Trim())
                .ToList();
        if (pluginsPath is null)
            Console.WriteLine("no plugins.txt found — base game + DLC + Creation Club only " +
                              "(pass a plugins.txt path as the profile argument)");
        else
            Console.WriteLine($"plugins.txt: {pluginsPath}");
        // Creation Club content loads via Skyrim.ccc (game root), between the
        // base masters and plugins.txt — the base-game AE mechanism, active
        // even with an empty plugins.txt (Steam Deck vanilla: 74 cc plugins
        // the old base-masters-only read missed, so their loot never
        // converted — Marth 2026-07-12).
        var ccc = new List<string>();
        var cccPath = Path.Combine(gameRoot, "Skyrim.ccc");
        if (File.Exists(cccPath))
        {
            ccc = File.ReadLines(cccPath)
                .Select(l => l.Trim())
                .Where(l => l.Length > 0 && !l.StartsWith('#'))
                .ToList();
            Console.WriteLine($"Skyrim.ccc: {ccc.Count} Creation Club plugin(s)");
        }
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var f in Directory.EnumerateFiles(dataDir))
            files.TryAdd(Path.GetFileName(f), f);
        var outList = new List<(string, string)>();
        missing = [];
        foreach (var p in BaseMasters.Concat(ccc).Concat(plugins)
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (files.TryGetValue(p, out var full)) outList.Add((p, full));
            else missing.Add(p);
        }
        return outList;
    }

    public static List<(string Name, string Path)> Resolve(
        string mo2Root, string profile, out List<string> missing)
    {
        if (!File.Exists(Path.Combine(mo2Root, "ModOrganizer.ini")) && IsGameRoot(mo2Root))
            return ResolveGame(mo2Root, profile, out missing);  // vanilla / manual install

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
