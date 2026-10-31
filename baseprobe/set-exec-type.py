"""Base probe: an eboot (fake SELF) as ET_SCE_EXEC (0xFE00), mapped where it was linked, instead of
create-fself's ET_SCE_DYNEXEC (0xFE10), which the PS4 maps at 0x400000. The fake SELF carries the ELF
header after its own header and segment table: its e_type is patched there.
  python3 set-exec-type.py eboot.bin"""
import struct
import sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
elf = data.find(b'\x7fELF')
assert elf >= 0, 'no ELF header in ' + path
(etype,) = struct.unpack_from('<H', data, elf + 0x10)
assert etype in (0xFE10, 0xFE00), 'unexpected e_type %#x' % etype
struct.pack_into('<H', data, elf + 0x10, 0xFE00)
open(path, 'wb').write(data)
print('%s: ELF at %#x, e_type %#x -> 0xfe00' % (path, elf, etype))
