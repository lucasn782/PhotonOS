from pathlib import Path
from elftools.elf.elffile import ELFFile
p = Path('shell.elf')
with p.open('rb') as f:
    elf = ELFFile(f)
    print('file size', p.stat().st_size)
    print('sections:')
    for sec in elf.iter_sections():
        print(sec.name, hex(sec['sh_addr']), hex(sec['sh_offset']), hex(sec['sh_size']))
    print('program headers:')
    for ph in elf.iter_segments():
        print(ph['p_type'], hex(ph['p_vaddr']), hex(ph['p_offset']), hex(ph['p_filesz']), hex(ph['p_memsz']), ph['p_flags'])
