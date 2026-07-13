#!/usr/bin/env python3
"""Scan a full MO2 load order for gem-candidate enchantments.

Dev-side reconnaissance for the install-time C# tool: resolves the MO2 VFS
(modlist.txt priority, Stock Game fallback), walks every enabled plugin for
ENCH/MGEF/WEAP/ARMO with load-order override semantics (last loader wins),
and reports single-effect enchantments whose MGEF is not already in
data/gem_catalog.json — grouped by MGEF, ranked by how many wearable items
actually carry them.

Usage: python3 tools/scan_loadorder.py <MO2 root> <profile> [out.json]
"""
import json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_ench import walk, parse_subrecords, gstr

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def resolve_load_order(mo2_root, profile):
    """Return [(plugin_name, path)] in load order, MO2-VFS-resolved."""
    prof = os.path.join(mo2_root, 'profiles', profile)
    if not os.path.exists(os.path.join(prof, 'plugins.txt')):
        have = [p for p in sorted(os.listdir(os.path.join(mo2_root, 'profiles')))
                if os.path.exists(os.path.join(mo2_root, 'profiles', p, 'plugins.txt'))]
        sys.exit(f"ERROR: no plugins.txt in profile '{profile}' (never launched?). "
                 f"Profiles with one: {have or 'none'}")
    plugins = [l[1:].strip() for l in open(os.path.join(prof, 'plugins.txt'),
               encoding='utf-8', errors='replace') if l.startswith('*')]
    mods = [l[1:].strip() for l in open(os.path.join(prof, 'modlist.txt'),
            encoding='utf-8', errors='replace')
            if l.startswith('+') and not l.rstrip().endswith('_separator')]
    # modlist.txt is stored highest-priority-first: first hit wins.
    search = [os.path.join(mo2_root, 'mods', m) for m in mods]
    # Stock-game fallback: Wabbajack lists name the bundled game dir all sorts
    # of ways; also honor gamePath from ModOrganizer.ini.
    for stock in ('Stock Game', 'Game Root', 'Stock Folder', 'Skyrim Stock',
                  'STOCK GAME', 'root'):
        d = os.path.join(mo2_root, stock, 'Data')
        if os.path.isdir(d):
            search.append(d)
    ini = os.path.join(mo2_root, 'ModOrganizer.ini')
    if os.path.exists(ini):
        for line in open(ini, encoding='utf-8', errors='replace'):
            if line.startswith('gamePath='):
                p = line.split('=', 1)[1].strip()
                if p.startswith('@ByteArray('):
                    p = p[len('@ByteArray('):].rstrip(')')
                p = p.replace('\\\\', '/').replace('\\', '/')
                if os.path.isdir(os.path.join(p, 'Data')):
                    search.append(os.path.join(p, 'Data'))
                break
    # Case-insensitive filename map per directory (Linux).
    dirmaps = []
    for d in search:
        try:
            dirmaps.append((d, {f.lower(): f for f in os.listdir(d)}))
        except OSError:
            continue
    # Base-game masters ship in Stock Game but aren't in plugins.txt.
    order = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm',
             'Dragonborn.esm'] + plugins
    out, missing = [], []
    for p in order:
        for d, fmap in dirmaps:
            real = fmap.get(p.lower())
            if real:
                out.append((p, os.path.join(d, real)))
                break
        else:
            missing.append(p)
    return out, missing


def scan(mo2_root, profile):
    lo, missing = resolve_load_order(mo2_root, profile)
    print(f"{len(lo)} plugins resolved, {len(missing)} missing", file=sys.stderr)
    if missing:
        print("  missing: " + ", ".join(missing[:10]), file=sys.stderr)
    if 'Skyrim.esm' in missing:
        sys.exit("ERROR: Skyrim.esm not found — not a Skyrim SE modlist "
                 "(or the stock game dir wasn't located).")

    mgefs, enchs, items = {}, {}, {}   # keyed (origin_master, local_fid)
    for i, (name, path) in enumerate(lo):
        if i % 200 == 0:
            print(f"  [{i}/{len(lo)}] {name}", file=sys.stderr)
        try:
            for rt, fid, fl, body, masters in walk(path, {'MGEF', 'ENCH', 'WEAP', 'ARMO'}):
                idx = fid >> 24
                origin = masters[idx] if idx < len(masters) else name
                key = (origin.lower(), fid & 0xFFFFFF)

                def res(formid):
                    oi = formid >> 24
                    oo = masters[oi] if oi < len(masters) else name
                    return (oo.lower(), formid & 0xFFFFFF)

                subs = parse_subrecords(body)
                edid = full = None
                d = {}
                effects = []
                cur = None
                for t, dat in subs:
                    if t == 'EDID': edid = gstr(dat)
                    elif t == 'FULL': full = gstr(dat)
                    elif t == 'ENIT': d['enit'] = dat
                    elif t == 'DATA' and rt == 'MGEF': d['mdata'] = dat
                    elif t == 'EITM' and len(dat) == 4:
                        d['eitm'] = res(struct.unpack('<I', dat)[0])
                    elif t == 'EFID':
                        cur = {'mgef': res(struct.unpack('<I', dat)[0])}
                    elif t == 'EFIT' and cur is not None:
                        mag, area, dur = struct.unpack('<fII', dat)
                        cur.update(mag=mag, area=area, dur=dur)
                        effects.append(cur); cur = None
                if rt == 'MGEF':
                    info = {'edid': edid, 'full': full, 'winner': name}
                    md = d.get('mdata', b'')
                    if len(md) >= 152:
                        info['flags'] = struct.unpack('<I', md[0:4])[0]
                        info['resistAV'] = struct.unpack('<i', md[16:20])[0]
                        info['archetype'] = struct.unpack('<I', md[64:68])[0]
                        info['primaryAV'] = struct.unpack('<i', md[68:72])[0]
                        info['castType'] = struct.unpack('<I', md[80:84])[0]
                        info['delivery'] = struct.unpack('<I', md[84:88])[0]
                        info['secondAV'] = struct.unpack('<i', md[88:92])[0]
                    mgefs[key] = info
                elif rt == 'ENCH':
                    e = {'edid': edid, 'full': full, 'effects': effects, 'winner': name}
                    enit = d.get('enit', b'')
                    if len(enit) >= 36:
                        (cost, flags, castType, amount, targetType, enchType,
                         chargeTime, baseEnch, worn) = struct.unpack('<IIIIIIfII', enit[:36])
                        e.update(castType=castType, targetType=targetType,
                                 enchType=enchType)
                    enchs[key] = e
                else:  # WEAP / ARMO
                    items[key] = {'type': rt, 'edid': edid, 'full': full,
                                  'ench': d.get('eitm'), 'winner': name}
        except Exception as ex:
            print(f"  PARSE FAIL {name}: {ex}", file=sys.stderr)

    # usage counts from winning item records only
    usage = {}
    for it in items.values():
        if it['ench']:
            usage.setdefault(it['ench'], []).append(it)

    cat = json.load(open(os.path.join(ROOT, 'data/gem_catalog.json')))
    known = {(r['plugin'].lower(), int(r['fid'], 16))
             for v in cat.values() for r in v['mgef_refs']}

    # Family signatures: what a gem's effect IS, independent of which MGEF
    # record a mod minted for it — derived from the catalog MGEFs' own winning
    # records, so it needs no hand-kept effect knowledge. archetype + primary
    # AV + hostile|detrimental bits identify the mechanic (catches Requiem's
    # separate "Fortify Magicka (Rank II)" MGEF as Fortify Magicka); resist AV
    # + second AV split the elements (fire/frost/shock/poison/unresistable all
    # share archetype+AV=Health and differ only there).
    def sig_of(m):
        if m.get('archetype') is None:
            return None
        return (m['archetype'], m['primaryAV'], m.get('flags', 0) & 0x5,
                m.get('resistAV'), m.get('secondAV'))

    covered_sigs = {}
    for gid, v in cat.items():
        for r in v['mgef_refs']:
            m = mgefs.get((r['plugin'].lower(), int(r['fid'], 16)))
            if m:
                s = sig_of(m)
                if s:
                    covered_sigs.setdefault(s, set()).add(v['name'])

    # Multi-effect enchantments can't map to a single gem; tally them
    # separately — the loot strip still has to decide their fate.
    multi = {}
    for key, e in enchs.items():
        if e.get('enchType') != 6 or len(e['effects']) <= 1:
            continue
        worn = usage.get(key, [])
        if worn:
            multi[key] = {'edid': e['edid'], 'full': e['full'],
                          'n_effects': len(e['effects']), 'items': len(worn),
                          'item_names': [f"{it['type']}:{it['full'] or it['edid']}"
                                         for it in worn[:3]]}

    groups = {}
    for key, e in enchs.items():
        if len(e['effects']) != 1 or e.get('enchType') != 6:
            continue
        mk = e['effects'][0]['mgef']
        if mk in known:
            continue
        m = mgefs.get(mk, {})
        gems = covered_sigs.get(sig_of(m) or (), None) if m else None
        # Keep-blacklist (marth 2026-07-09): named themed sets stay enchanted
        # even when single-effect and family-covered (artifact-class).
        KEEP_SETS = ('silentmoons',)
        blob = f"{m.get('edid') or ''} {e['edid'] or ''}".lower()
        kept_set = any(k in blob for k in KEEP_SETS)
        g = groups.setdefault(mk, {
            'mgef_edid': m.get('edid'), 'mgef_full': m.get('full'),
            'mgef_plugin': mk[0], 'mgef_fid': f"0x{mk[1]:06X}",
            'mgef_winner': m.get('winner'),
            'archetype': m.get('archetype'), 'primaryAV': m.get('primaryAV'),
            'castType': m.get('castType'), 'delivery': m.get('delivery'),
            'coverage': ('keep:set' if kept_set else
                         'family:' + '/'.join(sorted(gems)) if gems else 'new'),
            'enchs': [], 'items': 0, 'item_names': []})
        g['enchs'].append({'edid': e['edid'], 'full': e['full'],
                           'plugin': key[0], 'fid': f"0x{key[1]:06X}",
                           'winner': e['winner'], 'mag': e['effects'][0]['mag'],
                           'dur': e['effects'][0]['dur'],
                           'castType': e.get('castType'),
                           'targetType': e.get('targetType')})
        for it in usage.get(key, []):
            g['items'] += 1
            if len(g['item_names']) < 6 and (it['full'] or it['edid']):
                g['item_names'].append(f"{it['type']}:{it['full'] or it['edid']}")
    return groups, multi


def main():
    mo2_root = sys.argv[1] if len(sys.argv) > 1 else '/mnt/gaming/modlists/LoreRim'
    profile = sys.argv[2] if len(sys.argv) > 2 else 'Default'
    out = sys.argv[3] if len(sys.argv) > 3 else None
    groups, multi = scan(mo2_root, profile)
    ranked = sorted(groups.values(), key=lambda g: -g['items'])
    if out:
        json.dump({'singles': ranked,
                   'multi_effect': sorted(multi.values(), key=lambda m: -m['items'])},
                  open(out, 'w'), indent=1)
        print(f"wrote {out} ({len(ranked)} candidate MGEFs)", file=sys.stderr)

    DELIV = {0: 'self', 1: 'touch', 2: 'aimed', 3: 'targetActor', 4: 'targetLoc'}

    def show(rows, limit=40):
        print(f"{'items':>5}  {'delivery':<9} {'MGEF':<44} {'winner':<26} sample items")
        for g in rows[:limit]:
            deliv = DELIV.get(g.get('delivery'), '?')
            names = '; '.join(n.split(':', 1)[1] for n in g['item_names'][:3])
            print(f"{g['items']:>5}  {deliv:<9} {(g['mgef_edid'] or '?'):<44} "
                  f"{(g['mgef_winner'] or '?'):<26} {names[:70]}")

    new = [g for g in ranked if g['coverage'] == 'new']
    fam = [g for g in ranked if g['coverage'].startswith('family')]
    kept = [g for g in ranked if g['coverage'] == 'keep:set']
    big_new = [g for g in new if g['items'] >= 4]
    print(f"\n== NEW FAMILIES (no gem covers the effect; >=4 items = generic) "
          f"— {len(big_new)} big, {len(new) - len(big_new)} minor/artifact ==")
    show(big_new)
    print(f"\n== FAMILY-COVERED (different MGEF, same effect as an existing gem"
          f" — strip targets, not new gems) — {len(fam)} ==")
    for g in sorted(fam, key=lambda g: -g['items'])[:20]:
        print(f"{g['items']:>5}  {(g['mgef_edid'] or '?'):<44} -> {g['coverage'][7:]}")
    if kept:
        print(f"\n== KEEP-BLACKLISTED sets (artifact-class by decree) — {len(kept)} ==")
        for g in kept:
            print(f"{g['items']:>5}  {(g['mgef_edid'] or '?'):<44} "
                  f"{'; '.join(n.split(':', 1)[1] for n in g['item_names'][:3])[:60]}")
    n_multi_items = sum(m['items'] for m in multi.values())
    print(f"\n== MULTI-EFFECT enchantments on worn items (artifact-class per"
          f" marth 2026-07-09; tiered-generic ruling pending)"
          f" — {len(multi)} enchants / {n_multi_items} items ==")
    for m in sorted(multi.values(), key=lambda m: -m['items'])[:12]:
        names = '; '.join(n.split(':', 1)[1] for n in m['item_names'])
        print(f"{m['items']:>5}  x{m['n_effects']}  {(m['edid'] or '?'):<40} {names[:60]}")
    print(f"\n== MINOR/ARTIFACT new-effect MGEFs (<4 items, likely uniques) — "
          f"{len(new) - len(big_new)} (top 15 by items) ==")
    show(sorted([g for g in new if g['items'] < 4], key=lambda g: -g['items']), 15)


if __name__ == '__main__':
    main()
