#!/usr/bin/env python3
"""Prepare the pinned XOP sources for non-Linux POSIX sockets and select loop."""
from pathlib import Path
import shutil
import sys
source, destination = map(Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
for original in source.rglob('*'):
    if not original.is_file():
        continue
    path = destination / original.relative_to(source)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.suffix not in ('.h', '.cpp'):
        if not path.exists() or path.read_bytes() != original.read_bytes():
            shutil.copy2(original, path)
        continue
    text = original.read_text()
    text = text.replace('defined(__linux) || defined(__linux__)',
                        'defined(__linux) || defined(__linux__) || defined(__CYGWIN__) || defined(__APPLE__)')
    if path.name in ('EpollTaskScheduler.h', 'EpollTaskScheduler.cpp'):
        text = text.replace(' || defined(__CYGWIN__) || defined(__APPLE__)', '')
    if path.name == 'EventLoop.cpp':
        text = text.replace('new EpollTaskScheduler(n)', 'new SelectTaskScheduler(n)')
    if path.name == 'Socket.h':
        for header in ('netinet/ether.h', 'netpacket/packet.h', 'net/ethernet.h', 'net/route.h'):
            text = text.replace(f'#include <{header}>', '')
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
