"""Find the addresses XenonRecomp's config needs, by byte pattern, in a dumped image."""
import sys
BASE = 0x82000000
img = open(sys.argv[1], "rb").read()
pats = {
    "restgprlr_14_address": "e9c1ff68",
    "savegprlr_14_address": "f9c1ff68",
    "restfpr_14_address": "c9ccff70",
    "savefpr_14_address": "d9ccff70",
    "restvmx_14_address": "3960fee07dcb60ce",
    "savevmx_14_address": "3960fee07dcb61ce",
    "restvmx_64_address": "3960fc00100b60cb",
    "savevmx_64_address": "3960fc00100b61cb",
}
for name, hx in pats.items():
    p = bytes.fromhex(hx)
    hits, i = [], img.find(p)
    while i != -1:
        if i % 4 == 0:
            hits.append(BASE + i)
        i = img.find(p, i + 1)
    print(f"{name} = {', '.join(f'0x{h:08X}' for h in hits) or 'NOT FOUND'}")
