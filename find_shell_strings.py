import re
from pathlib import Path

path = Path('build/user/shell.elf')
with open(path, 'rb') as f:
    data = f.read()

strings = re.findall(rb'[ -~]{4,}', data)
for s in strings:
    if b'comando' in s or b'execve' in s or b'ping' in s or b'/bin/' in s or b'uso:' in s:
        print(s.decode(errors='replace'))
