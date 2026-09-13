"""Shared PyInstaller entry for the GUI and its console-free child workers."""
import multiprocessing
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def main():
    multiprocessing.freeze_support()
    if len(sys.argv) > 1 and sys.argv[1] in ('--run', '--gimbal', '--terrain', '--self-test'):
        mode = sys.argv.pop(1)
        if mode == '--run':
            from windows.runtime import main as worker
        elif mode == '--gimbal':
            from sitl.gimbal_sim import main as worker
        elif mode == '--terrain':
            from sitl.terrain_video import main as worker
        else:
            from windows.test_package import main as worker
        worker()
    else:
        from sitl_launch import main as launcher
        launcher()


if __name__ == '__main__':
    main()
