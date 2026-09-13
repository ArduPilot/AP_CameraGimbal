#!/usr/bin/env python3
"""Exercise the A8 boot supervisor with isolated paths and fake hardware."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix='a8-service-') as directory:
    root = Path(directory)
    runtime, app, config, commands = [root / name for name in ('run', 'customer/camera-app', 'config/camera-app', 'commands')]
    for path in (runtime, app, config, commands):
        path.mkdir(parents=True)
    original_config = '[recording]\nautorecord = while_armed\n'
    (config / 'camera.ini').write_text(original_config)
    (config / 'app_selection').write_text('vendor\n')
    (app / 'camera.ini.default').write_text('[recording]\nautorecord = false\n')
    (app / 'web.pass.default').write_text('test-password\n')
    source = root / 'fake.c'
    source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/file.h>
#include <fcntl.h>
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping=1; }
int main(int argc, char **argv) {
    (void)argc;
    int web = strstr(argv[0], "a8-web") != NULL;
    if (!web) {
        int fd = open(getenv("TEST_HARDWARE"), O_CREAT|O_RDWR, 0600);
        if (fd < 0 || flock(fd, LOCK_EX|LOCK_NB)) return 3;
        if (access(getenv("TEST_FAIL"), F_OK) == 0) return 2;
    }
    FILE *out = fopen(getenv(web ? "TEST_WEB_PID" : "CAMERA_APP_READY_PATH"), "w");
    if (!out) return 4;
    fprintf(out, "%d\n", getpid()); fclose(out);
    signal(SIGTERM, stop);
    while (!stopping) pause();
    return 0;
}
''')
    subprocess.run(['cc', str(source), '-o', str(app / 'camera-app')], check=True)
    (app / 'a8-web').write_bytes((app / 'camera-app').read_bytes())
    (app / 'a8-web').chmod(0o755)
    for name, status in (('ifconfig', 0), ('killall', 0), ('mount', 1), ('pidof', 1), ('sync', 0)):
        (commands / name).write_text(f'#!/bin/sh\nexit {status}\n')
        (commands / name).chmod(0o755)
    script = (REPO / 'packaging/a8/app_init.sh').read_text()
    # Replace source paths in one pass; never rewrite an already-expanded /tmp.
    import re
    paths = {'/customer': str(root / 'customer'), '/config': str(root / 'config'),
             '/tmp': str(runtime), '/mnt/mmc': str(root / 'media')}
    script = re.sub('|'.join(map(re.escape, paths)), lambda match: paths[match[0]], script)
    supervisor = root / 'supervisor.sh'
    supervisor.write_text(script)
    ready, web_pid, failure = runtime / 'camera-app.ready', runtime / 'web.pid', root / 'fail'
    env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
               TEST_HARDWARE=str(root / 'hardware'), TEST_FAIL=str(failure), TEST_WEB_PID=str(web_pid))
    log = (root / 'service.log').open('w')
    service = subprocess.Popen(['sh', str(supervisor)], env=env, stdout=log, stderr=log, start_new_session=True)

    def wait_for(predicate):
        deadline = time.monotonic() + 12
        while not predicate():
            assert service.poll() is None, (root / 'service.log').read_text()
            assert time.monotonic() < deadline, (root / 'service.log').read_text()
            time.sleep(.05)

    def request(generation):
        (runtime / 'camera-app.request').write_text(str(generation) + '\n')

    try:
        wait_for(lambda: ready.exists() and ready.stat().st_size > 0 and web_pid.exists() and web_pid.stat().st_size > 0)
        first = ready.read_text()
        old_web = web_pid.read_text()
        os.kill(int(old_web), signal.SIGTERM)
        wait_for(lambda: web_pid.read_text().strip().isdigit() and web_pid.read_text() != old_web)
        request(1)
        wait_for(lambda: ready.exists() and ready.stat().st_size > 0 and ready.read_text() != first)
        os.kill(int(ready.read_text()), signal.SIGTERM)
        wait_for(lambda: not ready.exists())
        time.sleep(1.2)
        assert not ready.exists(), 'crash must wait for an explicit restart'
        failure.touch()
        request(2)
        wait_for(lambda: (runtime / 'camera-app.started').read_text().strip() == '2')
        time.sleep(1.2)
        assert not ready.exists()
        os.kill(int(web_pid.read_text()), 0)
        failure.unlink()
        request(3)
        wait_for(ready.exists)
        assert (config / 'camera.ini').read_text() == original_config
        assert (config / 'app_selection').read_text() == 'vendor\n'
        print('PASS A8 supervisor: AP-only startup, saved settings, web respawn, restart, crash/startup failure recovery')
    finally:
        service.terminate()
        try:
            service.wait(timeout=12)
        finally:
            try:
                os.killpg(service.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            service.wait()
            log.close()
