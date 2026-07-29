from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = 'build/user/shell.elf'
with open(path, 'rb') as f:
    elf = ELFFile(f)
    print('ELF class:', elf['e_ident']['EI_CLASS'])
    print('Entry point:', hex(elf.header.e_entry))

    text = elf.get_section_by_name('.text')
    print('.text: addr=%s offset=%s size=%s' % (hex(text['sh_addr']), hex(text['sh_offset']), hex(text['sh_size'])))

    symbol_sections = []
    for name in ('.symtab', '.dynsym'):
        sec = elf.get_section_by_name(name)
        if sec:
            symbol_sections.append(sec)
            print('found symbol section', name, 'count', sec.num_symbols())

    if symbol_sections:
        for sec in symbol_sections:
            print('symbols from', sec.name)
            for sym in sec.iter_symbols():
                if sym['st_value'] and sym['st_size'] and sym['st_info']['type'] == 'STT_FUNC':
                    print('%s %s %s' % (hex(sym['st_value']), hex(sym['st_size']), sym.name))
    else:
        print('no symbol tables found')

    target_offset = 0x3483
    file_offset = target_offset
    if not (text['sh_offset'] <= file_offset < text['sh_offset'] + text['sh_size']):
        raise SystemExit('target offset not inside .text')
    addr = text['sh_addr'] + (file_offset - text['sh_offset'])
    print('mapped address for offset 0x3483:', hex(addr))

    f.seek(file_offset)
    data = f.read(32)
    print('bytes at 0x3483:', data.hex())

    start = max(text['sh_addr'], addr - 0x40)
    end = min(text['sh_addr'] + text['sh_size'], addr + 0x40)
    code = f.seek(text['sh_offset']) or f.read(text['sh_size'])
    f.seek(text['sh_offset'])
    code = f.read(text['sh_size'])
    code_slice = code[start - text['sh_offset']:end - text['sh_offset']]

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for insn in md.disasm(code_slice, start):
        print('0x%x\t%s\t%s' % (insn.address, insn.mnemonic, insn.op_str))
        if insn.address >= addr + 0x10:
            break
