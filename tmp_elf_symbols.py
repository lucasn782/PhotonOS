from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
p = Path('shell.elf')
with p.open('rb') as f:
    elf = ELFFile(f)
    print('entry', hex(elf.header.e_entry))
    for sec in elf.iter_sections():
        print('SEC', sec.name, 'addr', hex(sec['sh_addr']), 'off', hex(sec['sh_offset']), 'size', hex(sec['sh_size']))
    print('--- symbols ---')
    for sec in elf.iter_sections():
        if isinstance(sec, SymbolTableSection):
            print('SYMTAB', sec.name, 'num', sec.num_symbols())
            for sym in sec.iter_symbols():
                if sym['st_info']['type'] != 'STT_SECTION':
                    print(hex(sym['st_value']), sym.name, sym['st_size'], sym['st_info']['type'], sym['st_shndx'])
