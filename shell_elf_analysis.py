from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from pathlib import Path

path = Path('build/user/shell.elf')
assert path.exists(), path

data = path.read_bytes()
print('file size', len(data), 'bytes')

with open(path, 'rb') as f:
    elf = ELFFile(f)
    text = elf.get_section_by_name('.text')
    print('.text section: offset=', hex(text['sh_offset']), 'addr=', hex(text['sh_addr']), 'size=', hex(text['sh_size']))
    target_file_offset = 0x3483
    target_addr = text['sh_addr'] + (target_file_offset - text['sh_offset'])
    print('target file offset:', hex(target_file_offset), 'target addr:', hex(target_addr))
    start = max(text['sh_offset'], target_file_offset - 0x200)
    end = min(text['sh_offset'] + text['sh_size'], target_file_offset + 0x200)
    f.seek(start)
    code = f.read(end - start)
    base_addr = text['sh_addr'] + (start - text['sh_offset'])

md = Cs(CS_ARCH_X86, CS_MODE_64)
print('scanning function prologue backwards...')
for i in range(target_file_offset - text['sh_offset']):
    if i < 0 or i + 2 > len(code):
        continue
    # We'll search from start for push rbp; mov rbp, rsp and nearest before target.
    if i + 7 <= len(code) and code[i] == 0x55 and code[i+1] == 0x48 and code[i+2] == 0x89 and code[i+3] == 0xe5:
        instr_addr = base_addr + i
        if instr_addr <= target_addr:
            last_prologue = instr_addr
print('nearest prologue before target:', hex(last_prologue) if 'last_prologue' in locals() else 'none')

print('\ndisassembly around nearest prologue:')
if 'last_prologue' in locals():
    f = open(path, 'rb')
    off = text['sh_offset'] + (last_prologue - text['sh_addr'])
    f.seek(off)
    code = f.read(0x80)
    for insn in md.disasm(code, last_prologue):
        print('0x%x\t%s\t%s' % (insn.address, insn.mnemonic, insn.op_str))
        if insn.address >= target_addr:
            break
    f.close()

print('\ndisassembly window around target:')
f = open(path, 'rb')
off = text['sh_offset'] + (target_addr - text['sh_addr'])
f.seek(off - 0x20)
code = f.read(0x80)
base = target_addr - 0x20
for insn in md.disasm(code, base):
    marker = ' <-- target' if insn.address == target_addr else ''
    print('0x%x\t%s\t%s%s' % (insn.address, insn.mnemonic, insn.op_str, marker))
    if insn.address > target_addr + 0x20:
        break
f.close()
