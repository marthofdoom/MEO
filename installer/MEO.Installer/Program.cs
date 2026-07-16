// MEO.Installer — install-time patcher for marth's Enchanting Overhaul.
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
try
{
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
        if (args[i] is "--sub" or "--add")
        {
            if (i + 1 >= args.Length)
                return Commands.Fail($"{args[i]}: expects a following esp path");
            (args[i] == "--sub" ? subs : adds).Add(args[++i]);
        }
        else if (args[i] == "--no-mint-riders")
        {
            // Phase 3: single-effect minting only (the Synthesis patcher's
            // "Mint multi-effect recipes" setting, standalone spelling).
            Commands.MintMultiEffect = false;
        }
        else positional.Add(args[i]);
    }
}
}
catch (Exception ex)
{
    // Setup (MO2-root prompt, PickProfile, arg parse) reads the filesystem and
    // can throw BEFORE the load-order try below (missing profiles/ dir, bad
    // args). It sat outside any try, so a double-clicked Wine console died
    // silently — the m32e vanishing-window class. Surface it and hold the window.
    Console.Error.WriteLine("SETUP FAILED — MEO cannot determine the load order to read:");
    Console.Error.WriteLine(ex.ToString());
    return Pause(1, installMode);
}

List<(string Name, string Path)> resolved;
List<string> missing;
LoadOrder<IModListingGetter<ISkyrimModGetter>> loadOrder;
ILinkCache cache;
try
{
resolved = Mo2LoadOrder.Resolve(mo2Root, profile, out missing);
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
loadOrder = new LoadOrder<IModListingGetter<ISkyrimModGetter>>(listings);
cache = loadOrder.ToImmutableLinkCache();
Console.WriteLine($"load order read in {sw.Elapsed.TotalSeconds:F1}s");
}
catch (Exception ex)
{
    // m35 (audit): the load-order build (Resolve, overlay creation, LoadOrder)
    // reads the filesystem and is the MOST likely thing to throw in the field
    // (bad plugins.txt, missing modlist.txt, duplicate/corrupt esp). It sat
    // OUTSIDE the command try/catch, so under a double-clicked Wine console it
    // died silently — the m32e vanishing-window class. Never again.
    Console.Error.WriteLine("LOAD ORDER READ FAILED — MEO cannot continue:");
    Console.Error.WriteLine(ex.ToString());
    return Pause(1, installMode);
}

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
        "detect-candidates" => Commands.DetectCandidates(loadOrder, cache,
            positional.ElementAtOrDefault(0) ?? "data/gem_catalog.json",
            positional.ElementAtOrDefault(1) ?? "meo_candidates.json"),
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
        // converted — marth 2026-07-12).
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

        // m35 (audit): dedup — MO2 profiles routinely list base masters
        // (*Skyrim.esm) and can double-list a plugin; a duplicate ModKey makes
        // LoadOrder throw. ResolveGame already does this; parity here.
        var order = BaseMasters.Concat(plugins).Distinct(StringComparer.OrdinalIgnoreCase);
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
