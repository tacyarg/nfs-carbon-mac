"""Read and rewrite Need for Speed: Carbon's chunk files (.BUN/.BIN/.LZC) and the texture packs (TPK) in them.

Used at build time to apply texture mods to the app's own copy of the game (tools/setup_kit/hd_ui.py); the
player's game folder is never written to. The TPK layout follows Darius (github.com/nlgxzef/Darius, MIT,
Nikki/Support.Carbon/Class/TPKBlock.cs and Texture.cs); see CLAUDE.md "HD UI mod" for the field map.

    python tools/setup_kit/bun.py FILE          # round-trip check: parse, rewrite, compare
"""
import struct
import sys
from pathlib import Path

TPK, TPK_INFO, TPK_DATA = 0xB3300000, 0xB3310000, 0xB3320000
INFO_HEADER, INFO_KEYS, INFO_TEXTURES, INFO_FORMATS = 0x33310001, 0x33310002, 0x33310004, 0x33310005
DATA_HEADER, DATA_PIXELS = 0x33320001, 0x33320002
ALIGN = 0x80


def jdlz_decompress(src):
    """EA's JDLZ (header 'JDLZ' 02 10, uncompressed size at +8)."""
    size = struct.unpack_from("<I", src, 8)[0]
    out = bytearray()
    i, f1, f2 = 16, 1, 1
    while len(out) < size:
        if f1 == 1:
            f1 = src[i] | 0x100
            i += 1
        if f2 == 1:
            f2 = src[i] | 0x100
            i += 1
        if f1 & 1:
            if f2 & 1:
                length, back = (src[i + 1] | (src[i] & 0xF0) << 4) + 3, (src[i] & 0x0F) + 1
            else:
                back, length = (src[i + 1] | (src[i] & 0xE0) << 3) + 17, (src[i] & 0x1F) + 3
            i += 2
            for _ in range(length):
                out.append(out[-back])
            f2 >>= 1
        else:
            out.append(src[i])
            i += 1
        f1 >>= 1
    return bytes(out)


def load(path):
    """A chunk file's bytes, decompressed if it is JDLZ."""
    data = Path(path).read_bytes()
    return jdlz_decompress(data) if data[:4] == b"JDLZ" else data


def chunks(d, start=0, end=None):
    """(id, payload offset, payload size) for each chunk in d[start:end]."""
    end = len(d) if end is None else end
    o = start
    while o + 8 <= end:
        cid, size = struct.unpack_from("<II", d, o)
        yield cid, o + 8, size
        o += 8 + size


def padding(position, align=ALIGN):
    """A filler chunk (id 0) that brings `position` to the next `align` boundary (nothing if already there)."""
    gap = -position % align
    if gap == 0:
        return b""
    if gap < 8:
        gap += align
    return struct.pack("<II", 0, gap - 8) + bytes(gap - 8)


class Texture:
    """One texture: its header (0x58 bytes + name, padded to 4), format entry (0x18) and top-to-bottom data."""

    def __init__(self, header, fmt, data):
        self.header, self.fmt, self.data = bytearray(header), bytearray(fmt), data

    @property
    def name(self):
        n = self.header[0x58]
        return self.header[0x59:0x59 + n].split(b"\0")[0].decode("latin-1")

    @property
    def key(self):
        return struct.unpack_from("<I", self.header, 12)[0]

    @property
    def size(self):
        return struct.unpack_from("<hh", self.header, 40)


class Tpk:
    def __init__(self, d, o, size):
        self.info_header = self.anim = None
        self.extra_info = []  # other chunks in the info block, kept as they are
        keys = headers = formats = None
        data_header = data = None
        for cid, p, sz in chunks(d, o, o + size):
            if cid == TPK_INFO:
                for c2, p2, s2 in chunks(d, p, p + sz):
                    if c2 == INFO_HEADER:
                        self.info_header = d[p2:p2 + s2]
                    elif c2 == INFO_KEYS:
                        keys = d[p2:p2 + s2]
                    elif c2 == INFO_TEXTURES:
                        headers = (p2, s2)
                    elif c2 == INFO_FORMATS:
                        formats = (p2, s2)
                    else:
                        self.extra_info.append((c2, d[p2:p2 + s2]))
            elif cid == TPK_DATA:
                for c2, p2, s2 in chunks(d, p, p + sz):
                    if c2 == DATA_HEADER:
                        data_header = d[p2:p2 + s2]
                    elif c2 == DATA_PIXELS:
                        data = (p2 + ALIGN - 1) & ~(ALIGN - 1)
        if self.info_header is None or headers is None or formats is None or data is None:
            raise ValueError("not a complete TPK")
        self.data_header = data_header
        self.textures = []
        p, end, i = headers[0], headers[0] + headers[1], 0
        while p + 0x59 <= end:
            n = d[p + 0x58]
            h = d[p:p + 0x59 + n]
            off, sz = struct.unpack_from("<I", h, 20)[0], struct.unpack_from("<I", h, 28)[0]
            self.textures.append(Texture(h, d[formats[0] + 0x18 * i:formats[0] + 0x18 * (i + 1)],
                                         d[data + off:data + off + sz]))
            p += 0x59 + n
            i += 1

    @property
    def collection(self):
        return self.info_header[4:4 + 0x1C].split(b"\0")[0].decode("latin-1")

    def find(self, name):
        return next((t for t in self.textures if t.name.lower() == name.lower()), None)

    def assemble(self, position):
        """The TPK chunk's bytes, written at file offset `position` (alignment is absolute)."""
        out = bytearray()

        def chunk(cid, payload):
            out.extend(struct.pack("<II", cid, len(payload)))
            out.extend(payload)

        # Data offsets: each texture's data starts on a 0x80 boundary relative to the pixel base.
        offset = 0
        for t in self.textures:
            struct.pack_into("<I", t.header, 20, offset)
            struct.pack_into("<I", t.header, 28, len(t.data))
            offset += len(t.data) + (-len(t.data) % ALIGN)
        info = bytearray()
        for cid, payload in [(INFO_HEADER, self.info_header),
                             (INFO_KEYS, b"".join(struct.pack("<II", t.key, 0) for t in self.textures)),
                             (INFO_TEXTURES, b"".join(t.header for t in self.textures)),
                             (INFO_FORMATS, b"".join(t.fmt for t in self.textures))] + self.extra_info:
            info += struct.pack("<II", cid, len(payload)) + payload
        body = bytearray(struct.pack("<II", 0, 0x30) + bytes(0x30))
        body += struct.pack("<II", TPK_INFO, len(info)) + info
        body += padding(position + 8 + len(body))
        data = bytearray(struct.pack("<II", DATA_HEADER, len(self.data_header)) + self.data_header)
        data += struct.pack("<II", 0, 0x50) + bytes(0x50)
        pixels = bytearray(struct.pack("<I", 0x11111111) * 30)
        for t in self.textures:
            pixels += t.data + bytes(-len(t.data) % ALIGN)
        data += struct.pack("<II", DATA_PIXELS, len(pixels)) + pixels
        body += struct.pack("<II", TPK_DATA, len(data)) + data
        chunk(TPK, body)
        return bytes(out)


class ChunkFile:
    """A chunk file: top-level chunks kept as bytes, TPKs parsed. Filler chunks (id 0) are rebuilt on write
    so every other chunk keeps the start alignment (mod 0x80) it had."""

    def __init__(self, data):
        self.items = []  # (original offset % ALIGN, bytes | Tpk)
        for cid, p, size in chunks(data):
            if cid == 0:
                continue
            raw = data[p - 8:p + size]
            item = raw
            if cid == TPK:
                try:
                    item = Tpk(data, p, size)
                except ValueError:
                    item = raw
            self.items.append(((p - 8) % ALIGN, item))

    def tpks(self):
        return [item for _, item in self.items if isinstance(item, Tpk)]

    def tpk(self, collection):
        return next((t for t in self.tpks() if t.collection.lower() == collection.lower()), None)

    def assemble(self):
        out = bytearray()
        for phase, item in self.items:
            gap = (phase - len(out)) % ALIGN
            if gap:
                if gap < 8:
                    gap += ALIGN
                out += struct.pack("<II", 0, gap - 8) + bytes(gap - 8)
            out += item.assemble(len(out)) if isinstance(item, Tpk) else item
        return bytes(out)


if __name__ == "__main__":
    original = load(sys.argv[1])
    f = ChunkFile(original)
    rebuilt = f.assemble()
    print(f"{len(f.tpks())} TPKs: {', '.join(t.collection for t in f.tpks())}")
    print("identical" if rebuilt == original else f"differs: {len(original)} -> {len(rebuilt)} bytes")
