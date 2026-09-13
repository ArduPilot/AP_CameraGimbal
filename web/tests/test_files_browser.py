#!/usr/bin/env python3
"""Check Files sorting in a browser against an isolated SITL web server."""
import argparse
import html
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'sitl'))
from test_sitl import reserve_port, web_request
import socket

CHECKS = r"""
(() => {
  const result=document.createElement('pre');result.id='browser-test-result';document.body.append(result);
  try {
    const table=document.getElementById('files-table');
    const buttons=[...table.querySelectorAll('.file-sort')];
    const assert=(ok,message)=>{if(!ok)throw Error(message);};
    const names=()=>[...table.tBodies[0].rows].filter(r=>r.dataset.directory==='0').map(r=>r.cells[0].textContent);
    const ordered=expected=>assert(JSON.stringify(names())===JSON.stringify(expected),JSON.stringify(names()));
    ordered(['video1.mp4','video2.mp4','video10.mp4']);
    buttons[0].click();ordered(['video10.mp4','video2.mp4','video1.mp4']);
    buttons[2].click();ordered(['video2.mp4','video1.mp4','video10.mp4']);
    buttons[2].click();ordered(['video10.mp4','video1.mp4','video2.mp4']);
    buttons[3].click();ordered(['video10.mp4','video1.mp4','video2.mp4']);
    buttons[3].click();ordered(['video2.mp4','video1.mp4','video10.mp4']);
    buttons[4].click();ordered(['video2.mp4','video1.mp4','video10.mp4']);
    buttons[1].click();ordered(['video1.mp4','video2.mp4','video10.mp4']);
    buttons[1].click();ordered(['video1.mp4','video2.mp4','video10.mp4']);
    assert(table.tBodies[0].rows[0].dataset.parent==='1','parent directory moved');
    assert(table.tBodies[0].rows[1].dataset.directory==='1','directory not pinned');
    assert(buttons[1].parentElement.getAttribute('aria-sort')==='descending','missing sort direction');
    assert(sessionStorage.getItem('camera.files.sort')==='[1,-1]','sort preference not saved');
    buttons[3].focus();assert(document.activeElement===buttons[3],'header is not keyboard focusable');
    assert(table.querySelectorAll('a[href*="download=1"]').length===3,'downloads lost');
    result.textContent='PASS Files name/type/size/date/mode sorting, direction, directory order and persistence';
  }catch(e){result.textContent='FAIL '+e.stack;}
})();
"""


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('runtime',type=Path)
    args=parser.parse_args()
    browser=shutil.which('google-chrome') or shutil.which('chromium')
    assert browser, 'Chrome/Chromium required for browser test'
    runtime=args.runtime.resolve()
    subprocess.run([sys.executable,str(REPO/'sitl/prepare_runtime.py'),str(runtime),
                    str(REPO/'camera_app/camera.ini')],check=True)
    with tempfile.TemporaryDirectory(prefix='sort-',dir=runtime/'mnt') as directory, tempfile.TemporaryDirectory(prefix='files-browser-') as temporary:
        directory=Path(directory);temporary=Path(temporary)
        (directory/'folder').mkdir()
        for name,size,stamp,mode in (('video1.mp4',100,1600000002,0o644),
                                     ('video2.mp4',9,1600000003,0o600),
                                     ('video10.mp4',10000,1600000001,0o755)):
            p=directory/name;p.write_bytes(b'x'*size);p.chmod(mode);os.utime(p,(stamp,stamp))
        port=reserve_port(socket.SOCK_STREAM)
        with (temporary/'web.log').open('w') as log:
            web=subprocess.Popen([str(args.binary.resolve()),'-p',str(port)],stdout=log,stderr=subprocess.STDOUT)
            try:
                deadline=time.monotonic()+5
                while True:
                    try:
                        with web_request(port,'/files?path='+urllib.parse.quote(str(directory))) as response:
                            page=response.read().decode();csp=response.headers['Content-Security-Policy']
                        break
                    except OSError:
                        assert web.poll() is None and time.monotonic()<deadline
                        time.sleep(.05)
                assert 'script-src \'self\'' in csp and '<script src=/files.js defer>' in page
                with web_request(port,'/files.js') as response: script=response.read().decode()
            finally:
                web.terminate();web.wait(timeout=5)
        page=re.sub(r'<script\b[^>]*>.*?</script>','',page,flags=re.S)
        page=page.replace('</body>','<script>sessionStorage.clear();'+script+'</script><script>'+CHECKS+'</script></body>')
        fixture=temporary/'files.html';fixture.write_text(page)
        run=subprocess.run([browser,'--headless','--no-sandbox','--disable-gpu','--disable-dev-shm-usage',
                            '--no-first-run','--user-data-dir='+str(temporary/'profile'),
                            '--dump-dom',fixture.as_uri()],capture_output=True,timeout=30)
        match=re.search(r'<pre id="browser-test-result">(.*?)</pre>',run.stdout.decode(),re.S)
        assert run.returncode==0 and match,run.stderr.decode()[-2000:]
        result=html.unescape(match.group(1));assert result.startswith('PASS'),result
        print(result)


if __name__=='__main__':main()
