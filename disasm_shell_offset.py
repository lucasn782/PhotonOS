from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import os

path = 'build/user/shell.elf'
assert os.path.exists(path), path
with open(path, 'rb') as f:
    elf = ELFFile(f)
    text = elf.get_section_by_name('.text')
    offset = 0x3483
    file_offset = offset
    assert text['sh_offset'] <= file_offset < text['sh_offset'] + text['sh_size']
    addr = text['sh_addr'] + (file_offset - text['sh_offset'])
    print('text section:', hex(text['sh_addr']), 'file offset:', hex(text['sh_offset']), 'size:', hex(text['sh_size']))
    print('mapped address for offset 0x3483:', hex(addr))
    f.seek(file_offset)
    print('bytes at 0x3483:', f.read(16).hex())
    f.seek(text['sh_offset'])
    code = f.read(text['sh_size'])

md = Cs(CS_ARCH_X86, CS_MODE_64)
start = max(text['sh_addr'], addr - 0x40)
end = addr + 0x40
code_slice = code[start - text['sh_offset']:end - text['sh_offset']]
for insn in md.disasm(code_slice, start):
    print('0x%x\t%s\t%s' % (insn.address, insn.mnemonic, insn.op_str))
    if insn.address >= end:
        break
