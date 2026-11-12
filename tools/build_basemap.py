# 下地の地図（日本の都道府県の形と、まわりの国の陸地）を data/basemap.txt に取り出す。
#
#   python tools/build_basemap.py <ne_10m_admin_1_states_provinces.shp> > data/basemap.txt
#
# 元: Natural Earth 10m Admin 1 – States, Provinces（パブリックドメイン）
#     https://www.naturalearthdata.com/  ne_10m_admin_1_states_provinces.zip
# 出力:
#   pref <都道府県名>          続く ring がその都道府県
#   land <国コード>            続く ring がまわりの国（日本の外）
#   ring <lon>,<lat> ...       多角形の輪（間引き済み、小数 3 桁）
# 依存なし（shp と dbf は自前で読む）。
import struct, sys

BBOX = (118.0, 20.0, 156.0, 50.0)   # 経度・緯度の範囲（日本のまわり）
TOL = 0.004                          # 間引きの許容（度）。約 400 m


def read_dbf(path):
    with open(path, "rb") as f:
        head = f.read(32)
        n, hlen, rlen = struct.unpack("<IHH", head[4:12])
        fields = []
        while True:
            d = f.read(32)
            if d[0] == 0x0D:
                break
            fields.append((d[:11].split(b"\0")[0].decode(), d[11:12].decode(), d[16]))
        f.seek(hlen)
        rows = []
        for _ in range(n):
            r = f.read(rlen)
            pos, row = 1, {}
            for name, typ, ln in fields:
                row[name] = r[pos:pos + ln].decode("utf-8", "replace").strip(" \0")
                pos += ln
            rows.append(row)
    return rows


def read_shp(path):
    shapes = []
    with open(path, "rb") as f:
        f.seek(100)
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            _, clen = struct.unpack(">II", h)
            body = f.read(clen * 2)
            typ = struct.unpack("<i", body[:4])[0]
            if typ not in (5, 15, 25):
                shapes.append(None)
                continue
            box = struct.unpack("<4d", body[4:36])
            nparts, npts = struct.unpack("<ii", body[36:44])
            parts = list(struct.unpack(f"<{nparts}i", body[44:44 + 4 * nparts]))
            off = 44 + 4 * nparts
            pts = [struct.unpack("<2d", body[off + 16 * i: off + 16 * i + 16]) for i in range(npts)]
            rings = [pts[parts[k]:(parts[k + 1] if k + 1 < nparts else npts)] for k in range(nparts)]
            shapes.append((box, rings))
    return shapes


def simplify(pts, tol):
    if len(pts) < 4:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]; bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        L = (dx * dx + dy * dy) ** 0.5
        best, bi = -1, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / L if L > 0 else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > best:
                best, bi = d, i
        if best > tol:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]


def main(shp):
    rows = read_dbf(shp[:-4] + ".dbf")
    shapes = read_shp(shp)
    out = ["# 出典: Natural Earth（パブリックドメイン）ne_10m_admin_1_states_provinces を間引いたもの"]
    for row, sh in zip(rows, shapes):
        if sh is None:
            continue
        (x0, y0, x1, y1), rings = sh
        if x1 < BBOX[0] or x0 > BBOX[2] or y1 < BBOX[1] or y0 > BBOX[3]:
            continue
        if row.get("adm0_a3") == "JPN":
            out.append("pref " + (row.get("name_ja") or row.get("name")))
        else:
            out.append("land " + row.get("adm0_a3", "?"))
        for r in rings:
            s = simplify(r, TOL)
            if len(s) < 4:
                continue
            out.append("ring " + " ".join(f"{x:.3f},{y:.3f}" for x, y in s))
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
