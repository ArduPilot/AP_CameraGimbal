# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path
from PyInstaller.utils.hooks import collect_data_files, collect_submodules
root = Path(SPECPATH).parent
payload = root / 'build/windows/payload'
a = Analysis([str(root / 'windows/entry.py')], pathex=[str(root)],
             datas=[(str(payload / name), name) for name in ('native', 'fixtures', 'configs', 'licenses')] +
                   [(str(payload / 'targets.json'), '.')] +
                   collect_data_files('MAVProxy.modules.mavproxy_map'),
             hiddenimports=collect_submodules('MAVProxy.modules.mavproxy_map3d') +
                           ['MAVProxy.modules.lib.video_telemetry', 'pymavlink.dialects.v20.ardupilotmega',
                            'pymavlink.dialects.v10.ardupilotmega'],
             excludes=['PyQt5', 'PySide2', 'PySide6', 'tkinter'])
pyz = PYZ(a.pure)
gui = EXE(pyz, a.scripts, exclude_binaries=True, name='CameraGimbalSITL', console=False, upx=False)
worker = EXE(pyz, a.scripts, exclude_binaries=True, name='SITLWorker', console=True, upx=False)
COLLECT(gui, worker, a.binaries, a.datas, name='CameraGimbalSITL', upx=False)
