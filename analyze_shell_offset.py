import os
from elftools.elf.elffile import ELFFile

paths = ['build/user/shell.elf', 'shell.elf']
for path in paths:
    if not os.path.exists(path):
        continue
    print('===', path)
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        print('entry:', hex(elf.header.e_entry))
        text = elf.get_section_by_name('.text')
        rodata = elf.get_section_by_name('.rodata')
        print('.text:', text['sh_addr'], hex(text['sh_addr']), 'offset', hex(text['sh_offset']), 'size', hex(text['sh_size']))
        print('.rodata:', rodata['sh_addr'], hex(rodata['sh_addr']), 'offset', hex(rodata['sh_offset']), 'size', hex(rodata['sh_size']))
        off = 0x3483
        print('requested file offset:', hex(off))
        found = False
        for sec in elf.iter_sections():
            start = sec['sh_offset']
            end = start + sec['sh_size']
            if start <= off < end:
                print('in section', sec.name, 'section offset', hex(off - start), 'va', hex(sec['sh_addr'] + off - start))
                found = True
        for i, seg in enumerate(elf.iter_segments()):
            start = seg['p_offset']
            end = start + seg['p_filesz']
            if start <= off < end:
                print('in segment', i, 'type', seg['p_type'], 'seg va', hex(seg['p_vaddr'] + off - start), 'flags', seg['p_flags'])
                found = True
        if not found:
            print('offset not in any section or segment')
        f.seek(off)
        print('bytes at offset:', f.read(16).hex())

try:
    import capstone
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    print('capstone version', capstone.__version__)
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path,'rb') as f:
            elf = ELFFile(f)
            text = elf.get_section_by_name('.text')
            start = text['sh_addr']
            f.seek(text['sh_offset'])
            code = f.read(text['sh_size'])
            md = Cs(CS_ARCH_X86, CS_MODE_64)
            print('=== DISASSEMBLY', path)
            for i, insn in enumerate(md.disasm(code, start)):
                if i >= 120:
                    break
                if insn.address >= 0x8000001000 and insn.address < 0x8000001000 + 0x200:
                    print('0x%x\t%s\t%s' % (insn.address, insn.mnemonic, insn.op_str))
            print('--- near offset 0x3483 candidate')
            if 0x8000001000 <= 0x8000001000 + 0x3483 - 0x1000 < 0x8000001000 + len(code):
                pass
except Exception as e:
    print('capstone not available:', e)
