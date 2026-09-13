#!/usr/bin/env python3
"""Check secondary addressing and default routing in a disposable network namespace."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--inside', type=Path)
parser.add_argument('--sudo', action='store_true', help='use sudo for a network namespace when user namespaces are disabled')
args = parser.parse_args()
if args.inside:
    subprocess.run(['ip', 'link', 'add', 'ca-proxy-test', 'type', 'dummy'], check=True)
    subprocess.run(['ip', 'link', 'set', 'ca-proxy-test', 'up'], check=True)
    subprocess.run(['ip', 'addr', 'add', '198.51.100.25/24', 'dev', 'ca-proxy-test'], check=True)
    subprocess.run([str(args.inside)], check=True)
    addresses = json.loads(subprocess.check_output(['ip', '-j', '-4', 'addr', 'show', 'dev', 'ca-proxy-test']))
    assert {a['local'] for a in addresses[0]['addr_info']} == {'198.51.100.25', '192.0.2.25'}
    routes = json.loads(subprocess.check_output(['ip', '-j', '-4', 'route', 'show', 'default']))
    assert any(r.get('gateway') == '192.0.2.1' and r.get('dev') == 'ca-proxy-test' for r in routes), routes
    print('PASS existing camera address preserved and additional default route installed')
else:
    with tempfile.TemporaryDirectory(prefix='support-network-') as directory:
        binary = Path(directory) / 'test-network'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11',
            '-I' + str(REPO / 'camera_app/include'),
            '-I' + str(REPO / 'camera_app/build/mavlink/all/include'),
            str(REPO / 'camera_app/tests/test_support_network.c'),
            str(REPO / 'camera_app/src/protocol/support_network.c'), '-o', str(binary)], check=True)
        namespace = ['sudo', '-n', 'unshare', '--net'] if args.sudo else ['unshare', '--user', '--map-root-user', '--net']
        subprocess.run([*namespace, sys.executable,
                        str(Path(__file__).resolve()), '--inside', str(binary)], check=True)
