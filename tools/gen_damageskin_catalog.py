"""Generate the Cosmic damage-skin catalog seed from src/Custom.wz.

The skins live only in Custom.wz (Custom/BasicEff/BasicEff.img/damageSkin/<id>), which the server cannot
read, so the shop list is seeded into `damageskin_catalog` by a Liquibase changeset instead. Every skin that
has all of NoRed0/NoRed1/NoCri0/NoCri1 (what damageskin.cpp requires) gets a row at DEFAULT_PRICE.
Regenerate after changing the skins in Custom.wz; never hand-edit the SQL.

    python tools/gen_damageskin_catalog.py [--out PATH]

Needs pycryptodome (Custom.wz uses the GMS WZ key).
"""
import argparse
import pathlib
import struct

from Crypto.Cipher import AES

ROOT = pathlib.Path(__file__).resolve().parent.parent
CUSTOM_WZ = ROOT / "src" / "Custom.wz"
DEFAULT_OUT = ROOT.parent / "Server" / "src" / "main" / "resources" / "db" / "tables" / "028-damage-skin-catalog.sql"
SKIN_PATH = ("BasicEff", "BasicEff.img", "damageSkin")
REQUIRED = {"NoRed0", "NoRed1", "NoCri0", "NoCri1"}
DEFAULT_PRICE = 10_000_000
WZ_VERSION = "83"

USER_KEY = bytes(b for k in (0x13, 0x08, 0x06, 0xB4, 0x1B, 0x0F, 0x33, 0x52) for b in (k, 0, 0, 0))
GMS_IV = bytes((0x4D, 0x23, 0xC7, 0x2B))


def wz_key(size=0x4000):
    aes = AES.new(USER_KEY, AES.MODE_ECB)
    block, key = GMS_IV * 4, b""
    while len(key) < size:
        block = aes.encrypt(block)
        key += block
    return key


class Reader:
    def __init__(self, data, key):
        self.d = data
        self.key = key

    def cint(self, p):
        b = struct.unpack_from("<b", self.d, p)[0]
        if b == -128:
            return struct.unpack_from("<i", self.d, p + 1)[0], p + 5
        return b, p + 1

    def string(self, p):
        n = struct.unpack_from("<b", self.d, p)[0]
        p += 1
        if n == 0:
            return "", p
        k = self.key
        if n > 0:  # UTF-16
            if n == 127:
                n = struct.unpack_from("<i", self.d, p)[0]
                p += 4
            out = []
            for i in range(n):
                c = struct.unpack_from("<H", self.d, p)[0] ^ (0xAAAA + i) & 0xFFFF ^ (k[2 * i] | k[2 * i + 1] << 8)
                out.append(chr(c))
                p += 2
            return "".join(out), p
        n = -n
        if n == 128:
            n = struct.unpack_from("<i", self.d, p)[0]
            p += 4
        out = "".join(chr(self.d[p + i] ^ (0xAA + i) & 0xFF ^ k[i]) for i in range(n))
        return out, p + n


class WzFile(Reader):
    def __init__(self, path):
        super().__init__(path.read_bytes(), wz_key())
        assert self.d[:4] == b"PKG1", f"{path} is not a WZ file"
        self.start = struct.unpack_from("<I", self.d, 12)[0]
        self.hash = 0
        for c in WZ_VERSION:
            self.hash = self.hash * 32 + ord(c) + 1

    def offset(self, p):
        o = ((p - self.start) ^ 0xFFFFFFFF) * self.hash & 0xFFFFFFFF
        o = (o - 0x581C3F6D) & 0xFFFFFFFF
        s = o & 0x1F
        o = ((o << s) | (o >> (32 - s))) & 0xFFFFFFFF if s else o
        return ((o ^ struct.unpack_from("<I", self.d, p)[0]) + self.start * 2) & 0xFFFFFFFF

    def directory(self, p):
        """{name: (is_dir, offset)}"""
        n, p = self.cint(p)
        entries = {}
        for _ in range(n):
            kind = self.d[p]
            p += 1
            if kind == 2:
                ref = struct.unpack_from("<i", self.d, p)[0]
                p += 4
                kind = self.d[self.start + ref]
                name, _ = self.string(self.start + ref + 1)
            else:
                name, p = self.string(p)
            _, p = self.cint(p)  # size
            _, p = self.cint(p)  # checksum
            entries[name] = (kind == 3, self.offset(p))
            p += 4
        return entries


class Image(Reader):
    def __init__(self, data, key, base):
        super().__init__(data, key)
        self.base = base

    def sstring(self, p):
        tag = self.d[p]
        if tag in (0x00, 0x73):
            return self.string(p + 1)
        ref = struct.unpack_from("<i", self.d, p + 1)[0]
        return self.string(self.base + ref)[0], p + 5

    def properties(self, p):
        """{name: offset of the child's property list, or None}"""
        n, p = self.cint(p)
        out = {}
        for _ in range(n):
            name, p = self.sstring(p)
            kind = self.d[p]
            p += 1
            child = None
            if kind in (2, 11):
                p += 2
            elif kind in (3, 19):
                _, p = self.cint(p)
            elif kind == 20:
                p += 9 if struct.unpack_from("<b", self.d, p)[0] == -128 else 1
            elif kind == 4:
                p += 5 if self.d[p] == 0x80 else 1
            elif kind == 5:
                p += 8
            elif kind == 8:
                _, p = self.sstring(p)
            elif kind == 9:
                size = struct.unpack_from("<i", self.d, p)[0]
                p += 4
                obj, q = self.sstring(p)
                if obj == "Property":
                    child = q + 2
                p += size
            out[name] = child
        return out


def skin_ids():
    wz = WzFile(CUSTOM_WZ)
    offset = wz.start + 2
    for name in SKIN_PATH[:-2]:
        is_dir, offset = wz.directory(offset)[name]
        assert is_dir, f"{name} is not a directory"
    is_dir, img = wz.directory(offset)[SKIN_PATH[-2]]
    assert not is_dir
    image = Image(wz.d, wz.key, img)
    _, p = image.sstring(img)
    skins = image.properties(image.properties(p + 2)[SKIN_PATH[-1]])
    ids = []
    for name, child in skins.items():
        if name.isdigit() and int(name) > 0 and child is not None and REQUIRED <= image.properties(child).keys():
            ids.append(int(name))
    return sorted(ids)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    ids = skin_ids()
    lines = [
        "-- Damage skin shop catalog. GENERATED by kaentake/tools/gen_damageskin_catalog.py from Custom.wz",
        f"-- ({len(ids)} skins at {DEFAULT_PRICE:,} mesos). Do not edit by hand; change prices with UPDATE.",
        "INSERT IGNORE INTO damageskin_catalog (skinId, priceMesos)",
        "VALUES",
    ]
    lines += [f"    ({i}, {DEFAULT_PRICE}){',' if n < len(ids) - 1 else ';'}" for n, i in enumerate(ids)]
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {len(ids)} skins to {args.out}")


if __name__ == "__main__":
    main()
