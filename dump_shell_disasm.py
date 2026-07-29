from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = 'build/user/shell.elf'
target_offset = 0x3483

with open(path, 'rb') as f:
    f.seek(target_offset)
    data = f.read(64)
    print('raw bytes at offset', hex(target_offset), data.hex())

with open(path, 'rb') as f:
    elf = ELFFile(f)
    text = elf.get_section_by_name('.text')
    assert text['sh_offset'] <= target_offset < text['sh_offset'] + text['sh_size']
    addr = text['sh_addr'] + (target_offset - text['sh_offset'])
    print('mapped target addr', hex(addr))

md = Cs(CS_ARCH_X86, CS_MODE_64)
for insn in md.disasm(data, addr):
    marker = ' <-- target' if insn.address == addr else ''
    print('0x%x\t%s\t%s%s' % (insn.address, insn.mnemonic, insn.op_str, marker))
    if insn.address >= addr + 0x20:
        break
