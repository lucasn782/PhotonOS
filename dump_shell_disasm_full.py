from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = 'build/user/shell.elf'
target_offset = 0x3483
range_back = 0x80
range_forward = 0x80

with open(path, 'rb') as f:
    elf = ELFFile(f)
    text = elf.get_section_by_name('.text')
    assert text['sh_offset'] <= target_offset < text['sh_offset'] + text['sh_size']
    addr = text['sh_addr'] + (target_offset - text['sh_offset'])
    start = max(text['sh_offset'], target_offset - range_back)
    end = min(text['sh_offset'] + text['sh_size'], target_offset + range_forward)
    f.seek(start)
    code = f.read(end - start)

md = Cs(CS_ARCH_X86, CS_MODE_64)
base_addr = text['sh_addr'] + (start - text['sh_offset'])
print('file offset range', hex(start), hex(end), 'base addr', hex(base_addr), 'target addr', hex(addr))
for insn in md.disasm(code, base_addr):
    marker = ' <-- target' if insn.address == addr else ''
    print('0x%x\t%s\t%s%s' % (insn.address, insn.mnemonic, insn.op_str, marker))
    if insn.address > addr + 0x40:
        break
