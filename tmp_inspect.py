from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
p=Path('shell.elf')
with p.open('rb') as f:
    elf=ELFFile(f)
    print('entry', hex(elf.header['e_entry']))
    for sec in elf.iter_sections():
        print('SEC', sec.name, 'addr', hex(sec['sh_addr']), 'off', hex(sec['sh_offset']), 'size', hex(sec['sh_size']))
    targ=0x3483
    print('target file offset', hex(targ))
    for sec in elf.iter_sections():
        start=sec['sh_offset']; end=start+sec['sh_size']
        if targ>=start and targ<end:
            print('target in section', sec.name, 'vaddr', hex(sec['sh_addr'] + (targ-start)))
    f.seek(targ)
    data=f.read(64)
    print('bytes', data.hex())
    print('disasm around target file offset')
    md=Cs(CS_ARCH_X86, CS_MODE_64)
    for ins in md.disasm(data, 0x8000000000+targ):
        print(hex(ins.address), ins.mnemonic, ins.op_str)
