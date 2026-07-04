#!/usr/bin/env python3
"""Parse ENCH + MGEF records from Skyrim masters to build the gem catalog."""
import struct, zlib, sys, json, os

DATA = "/mnt/gaming/Steam/steamapps/common/Skyrim Special Edition/Data"
MASTERS = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]

def parse_subrecords(data):
    subs = []
    i = 0
    while i + 6 <= len(data):
        stype = data[i:i+4].decode('ascii', 'replace')
        size = struct.unpack('<H', data[i+4:i+6])[0]
        if stype == 'XXXX':
            size = struct.unpack('<I', data[i+6:i+10])[0]
            i += 10
            stype = data[i:i+4].decode('ascii', 'replace')
            i += 6
            subs.append((stype, data[i:i+size]))
            i += size
            continue
        subs.append((stype, data[i+6:i+6+size]))
        i += 6 + size
    return subs

def walk(path, want_types):
    """Yield (rtype, formid, flags, data, masters) for records in top-level GRUPs."""
    with open(path, 'rb') as f:
        blob = f.read()
    dsize = struct.unpack('<I', blob[4:8])[0]
    tes4_data = blob[24:24+dsize]
    pos = 24 + dsize
    masters = [d.decode('ascii','replace').rstrip('\x00') for t,d in parse_subrecords(tes4_data) if t=='MAST']
    n = len(blob)
    while pos + 24 <= n:
        rtype = blob[pos:pos+4]
        if rtype == b'GRUP':
            gsize = struct.unpack('<I', blob[pos+4:pos+8])[0]
            label = blob[pos+8:pos+12]
            if label.decode('ascii','replace') in want_types:
                end = pos + gsize
                p = pos + 24
                while p + 24 <= end:
                    rt = blob[p:p+4]
                    if rt == b'GRUP':
                        gs = struct.unpack('<I', blob[p+4:p+8])[0]
                        p += gs
                        continue
                    ds, fl, fid = struct.unpack('<III', blob[p+4:p+16])
                    body = blob[p+24:p+24+ds]
                    if fl & 0x00040000:
                        body = zlib.decompress(body[4:])
                    yield rt.decode(), fid, fl, body, masters
                    p += 24 + ds
            pos += gsize
        else:
            dsize = struct.unpack('<I', blob[pos+4:pos+8])[0]
            pos += 24 + dsize

def gstr(d):
    return d.rstrip(b'\x00').decode('cp1252', 'replace')

def build_catalog():
    """Return (mgefs dict, enchs list) parsed from the game masters."""
    mgefs = {}
    enchs = []
    for m in MASTERS:
        path = os.path.join(DATA, m)
        if not os.path.exists(path):
            print(f"MISSING {m}", file=sys.stderr); continue
        for rt, fid, fl, body, masters in walk(path, {'MGEF', 'ENCH'}):
            idx = fid >> 24
            origin = masters[idx] if idx < len(masters) else m
            key = (origin, fid & 0xFFFFFF)
            subs = parse_subrecords(body)
            d = {}
            edid = full = None
            effects = []
            cur = None
            for t, dat in subs:
                if t == 'EDID': edid = gstr(dat)
                elif t == 'FULL': full = gstr(dat)
                elif t == 'ENIT': d['enit'] = dat
                elif t == 'DATA' and rt == 'MGEF': d['mdata'] = dat
                elif t == 'EFID':
                    midx = struct.unpack('<I', dat)[0]
                    oi = midx >> 24
                    oo = masters[oi] if oi < len(masters) else m
                    cur = {'mgef': (oo, midx & 0xFFFFFF)}
                elif t == 'EFIT' and cur is not None:
                    mag, area, dur = struct.unpack('<fII', dat)
                    cur.update(mag=mag, area=area, dur=dur)
                    effects.append(cur); cur = None
            if rt == 'MGEF':
                md = d.get('mdata', b'')
                info = {'edid': edid, 'full': full}
                if len(md) >= 152:
                    info['flags'] = struct.unpack('<I', md[0:4])[0]
                    info['archetype'] = struct.unpack('<I', md[64:68])[0]
                    info['primaryAV'] = struct.unpack('<i', md[68:72])[0]
                    info['castType'] = struct.unpack('<I', md[80:84])[0]
                    info['delivery'] = struct.unpack('<I', md[84:88])[0]
                    info['secondAV'] = struct.unpack('<i', md[88:92])[0]
                mgefs[key] = info
            else:
                enit = d.get('enit', b'')
                e = {'plugin': m, 'fid': fid & 0xFFFFFF, 'origin': origin,
                     'edid': edid, 'full': full, 'effects': effects}
                if len(enit) >= 36:
                    (cost, flags, castType, amount, targetType, enchType,
                     chargeTime, baseEnch, wornRestrict) = struct.unpack('<IIIIIIfII', enit[:36])
                    e.update(cost=cost, flags=flags, castType=castType, amount=amount,
                             targetType=targetType, enchType=enchType,
                             baseEnch=baseEnch & 0xFFFFFF if baseEnch else 0)
                enchs.append(e)
    # later master wins for same (origin, local fid)
    seen = {}
    for e in enchs:
        seen[(e['origin'], e['fid'])] = e
    return mgefs, list(seen.values())

def main():
    mgefs, enchs = build_catalog()
    print(json.dumps({'mgef_count': len(mgefs), 'ench_count': len(enchs)}))
    from collections import Counter
    print("enchType counts:", Counter(e.get('enchType') for e in enchs))
    print("castType counts:", Counter(e.get('castType') for e in enchs))
    cat = []
    for e in sorted(enchs, key=lambda x: (x.get('edid') or '')):
        effs = []
        for ef in e['effects']:
            mg = mgefs.get(ef['mgef'], {})
            effs.append({'mgef_edid': mg.get('edid'), 'mgef_full': mg.get('full'),
                         'mgef_origin': ef['mgef'][0], 'mgef_fid': f"0x{ef['mgef'][1]:06X}",
                         'archetype': mg.get('archetype'), 'primaryAV': mg.get('primaryAV'),
                         'mag': ef.get('mag'), 'area': ef.get('area'), 'dur': ef.get('dur')})
        cat.append({'edid': e['edid'], 'full': e['full'], 'origin': e['origin'],
                    'fid': f"0x{e['fid']:06X}", 'enchType': e.get('enchType'),
                    'castType': e.get('castType'), 'targetType': e.get('targetType'),
                    'baseEnch': f"0x{e.get('baseEnch',0):06X}", 'effects': effs})
    with open(sys.argv[1] if len(sys.argv) > 1 else '/dev/stdout', 'w') as f:
        json.dump(cat, f, indent=1)
    print(f"wrote {len(cat)} enchantments")

if __name__ == '__main__':
    main()
