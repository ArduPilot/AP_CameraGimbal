#!/usr/bin/env python3
"""Collect the Cygwin DLL closure without copying Windows system libraries."""
from pathlib import Path
import shutil
import subprocess
import sys
root = Path(sys.argv[1])
for executable in list(root.glob('*.exe')):
    text = subprocess.check_output(['cygcheck', str(executable)], text=True)
    for line in text.splitlines():
        path = line.strip()
        if not path.lower().endswith('.dll'):
            continue
        posix = Path(subprocess.check_output(['cygpath', '-u', path], text=True).strip())
        if posix.parent == Path('/usr/bin') or posix.parent == Path('/bin'):
            shutil.copy2(posix, root / posix.name)
        elif posix.name.lower().startswith('cyg') and posix.exists():
            shutil.copy2(posix, root / posix.name)
if not (root / 'cygwin1.dll').is_file():
    raise RuntimeError('Cygwin runtime was not collected')
notices = root.parent / 'cygwin-licenses'
notices.mkdir(exist_ok=True)
for name in ('COPYING', 'CYGWIN_LICENSE'):
    shutil.copy2(Path('/usr/share/doc/Cygwin') / name, notices / name)
(notices / 'packages.txt').write_text(subprocess.check_output(['cygcheck', '-c'], text=True))
