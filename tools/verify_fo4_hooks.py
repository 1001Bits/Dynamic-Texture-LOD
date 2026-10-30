"""Check call sites against installed executables without running the games."""
import argparse
import json
import os
from pathlib import Path
import re
import struct

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
# The folder holding version-<runtime>.bin (Data/F4SE/Plugins of an install with Address Library for F4SE
# Plugins), from --address-library or F4_ADDRESS_LIBRARY.
LIBRARY = None


def inspect(path):
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    info = pe.VS_FIXEDFILEINFO[0]
    version = (info.FileVersionMS >> 16, info.FileVersionMS & 65535,
               info.FileVersionLS >> 16, info.FileVersionLS & 65535)
    vr = version[:3] == (1, 2, 72)
    ng = version >= (1, 10, 980, 0) and not vr
    base = pe.OPTIONAL_HEADER.ImageBase
    section = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    code = section.get_data()
    start = base + section.VirtualAddress
    mappings = {} if vr else dict(struct.iter_unpack(
        '<QQ', (LIBRARY / ('version-' + '-'.join(map(str, version)) + '.bin')).read_bytes()[8:]))
    locations = {}
    source = (ROOT / 'src/Addresses.h').read_text(encoding='utf-8')
    for name, og, modern, offset in re.findall(r'Location\s+(\w+)\s*\{\s*(\d+),\s*(\d+),\s*(0x[0-9A-Fa-f]+)\s*\}', source):
        locations[name] = base + (int(offset, 16) if vr else mappings[int(modern if ng else og)])

    def calls(target):
        found = []
        index = code.find(b'\xe8')
        while index >= 0 and index + 5 <= len(code):
            if start + index + 5 + struct.unpack_from('<i', code, index + 1)[0] == target:
                found.append(start + index)
            index = code.find(b'\xe8', index + 1)
        return found

    desired = calls(locations['GetDesiredDegradeLevel'])
    updates = calls(locations['UpdateRec'])
    rounds = calls(locations['StartTextureRound'])
    textures = [a for a in calls(locations['GetTextureMaxDegradeLevel'])
                if locations['TextureFunctor'] <= a < locations['TextureFunctor'] + 0x220]
    assert 8 <= len(desired) <= 24, ('desired', len(desired))
    assert 3 <= len(updates) <= 48, ('updates', len(updates))
    assert 1 <= len(rounds) <= 8, ('rounds', len(rounds))
    assert 1 <= len(textures) <= 4, ('textures', len(textures))
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    functor_rva = locations['TextureFunctor'] - base
    functor_prologue = bytes.fromhex('405341564157' if ng else '4053554156')
    assert pe.get_data(functor_rva, len(functor_prologue)) == functor_prologue
    replay = list(disassembler.disasm(functor_prologue, locations['TextureFunctor']))
    assert sum(i.size for i in replay) == len(functor_prologue) and all(i.mnemonic == 'push' for i in replay)
    visits = []
    slot = 0x36 if vr else 0x35
    pattern = b'\xff\x90' + struct.pack('<I', slot * 8)
    for function, size in [('UpdateRec', 0x400), ('UpdateAlwaysRender', 0x480)]:
        address = locations[function]
        level = next(a for a in desired if address <= a < address + size)
        visit_offset = code.find(pattern, level - start + 5, level - start + 45)
        assert level + 5 <= start + visit_offset < level + 40, (function, 'texture visit missing')
        visit = start + visit_offset
        decoded = {i.address: i for i in disassembler.disasm(code[address-start:address-start+size], address)}
        assert level in decoded and decoded[level].mnemonic == 'call'
        assert visit in decoded and decoded[visit].mnemonic == 'call' and decoded[visit].size == 6
        visits.append({'function': function, 'level': hex(level), 'visit': hex(visit), 'slot': slot})
    result = {'file': str(path), 'version': '.'.join(map(str, version)),
              'desired_calls': len(desired), 'texture_calls': len(textures),
              'update_calls': len(updates), 'round_calls': len(rounds), 'visits': visits,
              'owner_functor_entry': hex(locations['TextureFunctor']), 'owner_functor_replay': functor_prologue.hex()}
    pe.close()
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executables', nargs='+', type=Path)
    parser.add_argument('--address-library', type=Path, default=os.environ.get('F4_ADDRESS_LIBRARY'),
                        help='folder with version-<runtime>.bin (default: F4_ADDRESS_LIBRARY)')
    args = parser.parse_args()
    if not args.address_library or not Path(args.address_library).is_dir():
        parser.error('give --address-library or set F4_ADDRESS_LIBRARY')
    LIBRARY = Path(args.address_library)
    print(json.dumps([inspect(path) for path in args.executables], indent=2))
