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
    plugins = [l[1:].strip() for l in open(os.path.join(prof, 'plugins.txt'),
               encoding='utf-8', errors='replace') if l.startswith('*')]
    mods = [l[1:].strip() for l in open(os.path.join(prof, 'modlist.txt'),
            encoding='utf-8', errors='replace')
            if l.startswith('+') and not l.rstrip().endswith('_separator')]
    # modlist.txt is stored highest-priority-first: first hit wins.
    search = [os.path.join(mo2_root, 'mods', m) for m in mods]
    search.append(os.path.join(mo2_root, 'Stock Game', 'Data'))
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
                        info['archetype'] = struct.unpack('<I', md[64:68])[0]
                        info['primaryAV'] = struct.unpack('<i', md[68:72])[0]
                        info['castType'] = struct.unpack('<I', md[80:84])[0]
                        info['delivery'] = struct.unpack('<I', md[84:88])[0]
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

    groups = {}
    for key, e in enchs.items():
        if len(e['effects']) != 1 or e.get('enchType') != 6:
            continue
        mk = e['effects'][0]['mgef']
        if mk in known:
            continue
        m = mgefs.get(mk, {})
        g = groups.setdefault(mk, {
            'mgef_edid': m.get('edid'), 'mgef_full': m.get('full'),
            'mgef_plugin': mk[0], 'mgef_fid': f"0x{mk[1]:06X}",
            'mgef_winner': m.get('winner'),
            'archetype': m.get('archetype'), 'primaryAV': m.get('primaryAV'),
            'castType': m.get('castType'), 'delivery': m.get('delivery'),
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
    return groups


def main():
    mo2_root = sys.argv[1] if len(sys.argv) > 1 else '/mnt/gaming/modlists/LoreRim'
    profile = sys.argv[2] if len(sys.argv) > 2 else 'Default'
    out = sys.argv[3] if len(sys.argv) > 3 else None
    groups = scan(mo2_root, profile)
    ranked = sorted(groups.values(), key=lambda g: -g['items'])
    if out:
        json.dump(ranked, open(out, 'w'), indent=1)
        print(f"wrote {out} ({len(ranked)} candidate MGEFs)", file=sys.stderr)
    print(f"\n{len(ranked)} single-effect weapon/armor enchant MGEFs not in the gem catalog")
    print(f"{'items':>5}  {'delivery':<9} {'MGEF':<44} {'winner':<28} sample enchants / items")
    DELIV = {0: 'self', 1: 'touch', 2: 'aimed', 3: 'targetActor', 4: 'targetLoc'}
    for g in ranked[:80]:
        deliv = DELIV.get(g.get('delivery'), '?')
        names = '; '.join(g['item_names'][:3])
        print(f"{g['items']:>5}  {deliv:<9} {(g['mgef_edid'] or '?'):<44} "
              f"{(g['mgef_winner'] or '?'):<28} {names[:70]}")


if __name__ == '__main__':
    main()
