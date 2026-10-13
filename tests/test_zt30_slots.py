#!/usr/bin/env python3
"""Check slot publication, interrupted migration and coherent launcher selection."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Slots(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='zt30-slots-build-')
        cls.binary = Path(cls.build.name)/'zt30-slot'
        subprocess.run(['c++','-std=gnu++17','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),
                        str(ROOT/'tools/zt30_slots.cpp'),'-o',str(cls.binary)],check=True)
        listener = Path(cls.build.name)/'listener.cpp'
        listener.write_text(r'''#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char **argv) {
 if(argc!=3 || strcmp(argv[1],"-p")) return 2;
 FILE *f=fopen(getenv("SLOT_EVENTS"),"a");if(!f)return 3;
 fprintf(f,"web %ld %s fd9=%d\n",(long)getpid(),getenv("CAMERA_APP_WEBROOT"),fcntl(9,F_GETFD));fclose(f);
 while(true)pause();
}''')
        cls.listener = Path(cls.build.name)/'zt30-web'
        subprocess.run(['c++','-O2',str(listener),'-o',str(cls.listener)],check=True)
        cls.listener.chmod(0o755)
    @classmethod
    def tearDownClass(cls): cls.build.cleanup()
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zt30-slots-')
        self.root = Path(self.temp.name)/'app'; self.root.mkdir(mode=0o755)
        self.payload(self.root)
        (self.root/'camera.ini').write_text('user config\n')
        (self.root/'auth').mkdir(mode=0o700)
        (self.root/'auth/key').write_text('identity fixture\n')
    def tearDown(self): self.temp.cleanup()
    def payload(self,path):
        path.mkdir(parents=True,exist_ok=True,mode=0o755)
        for name in ('camera-app','zt30-web','encoder-sdk.so'):
            (path/name).write_text('fixture '+name+'\n');(path/name).chmod(0o755)
        (path/'webroot').mkdir(mode=0o755)
        for name in ('live.js','status-actions.html','parameters-form.html'):
            (path/'webroot'/name).write_text('asset '+name+'\n');(path/'webroot'/name).chmod(0o644)
    def slot(self,id):
        (self.root/'slots').mkdir(exist_ok=True,mode=0o755)
        path=self.root/'slots'/id;self.payload(path);return path
    def call(self,*args,ok=True):
        p=subprocess.run([str(self.binary),'--root',str(self.root),*args],capture_output=True,text=True)
        self.assertEqual(p.returncode==0,ok,p.stderr)
        return p.stdout.strip()
    def test_migrate_switch_and_explicit_rollback(self):
        old=self.slot('a'*64);new=self.slot('b'*64)
        self.assertEqual(self.call('resolve'),str(self.root))
        before=[(self.root/x).read_bytes() for x in ('camera.ini','auth/key')]
        self.call('migrate','a'*64)
        self.assertEqual(self.call('resolve'),str(old))
        for name in ('camera-app','zt30-web','encoder-sdk.so'):
            self.assertEqual(os.readlink(self.root/name),'active/'+name)
        self.call('activate','b'*64)
        self.assertEqual(self.call('resolve'),str(new))
        self.call('activate','a'*64)
        self.assertEqual(self.call('resolve'),str(old))
        self.assertEqual(before,[(self.root/x).read_bytes() for x in ('camera.ini','auth/key')])
        self.assertTrue(new.is_dir()) # no garbage collection of potentially running slots
    def test_interrupted_alias_migration_is_idempotent(self):
        self.slot('a'*64)
        self.call('activate','a'*64)
        (self.root/'camera-app').unlink();(self.root/'camera-app').symlink_to('active/camera-app')
        self.call('migrate','a'*64)
        self.call('migrate','a'*64)
        for name in ('camera-app','zt30-web','encoder-sdk.so'):
            self.assertEqual(os.readlink(self.root/name),'active/'+name)
    def test_migration_must_match_legacy_and_retain_active(self):
        a=self.slot('a'*64);self.slot('b'*64)
        (a/'camera-app').write_text('different')
        self.call('migrate','a'*64,ok=False)
        self.assertFalse((self.root/'active').exists())
        self.call('activate','b'*64)
        self.call('migrate','a'*64,ok=False)
        self.assertEqual(os.readlink(self.root/'active'),'slots/'+'b'*64)
    def test_bad_payload_cannot_change_current_or_fall_back(self):
        self.slot('a'*64);bad=self.slot('b'*64)
        self.call('activate','a'*64)
        for id in ('../outside','A'*64,'b'*63,'b'*64+'/extra'):
            self.call('activate',id,ok=False)
            self.assertEqual(os.readlink(self.root/'active'),'slots/'+'a'*64)
        (bad/'camera-app').unlink();(bad/'camera-app').symlink_to(self.root/'camera-app')
        self.call('activate','b'*64,ok=False)
        self.assertEqual(os.readlink(self.root/'active'),'slots/'+'a'*64)
        (self.root/'active').unlink();(self.root/'active').symlink_to('slots/'+'b'*64)
        self.call('resolve',ok=False) # never run flat/vendor payload if active is bad
    def test_symlink_directories_extra_entries_and_write_modes(self):
        slot=self.slot('a'*64);other=Path(self.temp.name)/'outside'
        slot.rename(other);slot.symlink_to(other,target_is_directory=True)
        self.call('activate','a'*64,ok=False)
        slot.unlink();other.rename(slot)
        (slot/'unexpected').mkdir();self.call('activate','a'*64,ok=False);(slot/'unexpected').rmdir()
        (slot/'webroot/live.js').chmod(0o666);self.call('activate','a'*64,ok=False)
        (slot/'webroot/live.js').chmod(0o644)
        (self.root/'active').write_text('not a symlink')
        self.call('resolve',ok=False)
    def test_writer_lock_does_not_modify_active(self):
        self.slot('a'*64)
        with (self.root/'.slot-update.lock').open('w') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            self.call('activate','a'*64,ok=False)
            self.assertFalse((self.root/'active').exists())

    def test_boot_prefix_starts_recovery_even_with_invalid_slot(self):
        run=Path(self.temp.name)/'run';run.mkdir()
        fake=Path(self.temp.name)/'bin';fake.mkdir()
        (fake/'ifconfig').write_text('#!/bin/sh\nexit 0\n');(fake/'ifconfig').chmod(0o755)
        events=Path(self.temp.name)/'events'
        (self.root/'web.pass.default').write_text('fixture\n')
        (self.root/'start-dropbear.sh').write_text('#!/bin/sh\necho recovery >> "$SLOT_EVENTS"\n')
        (self.root/'start-dropbear.sh').chmod(0o755)
        (self.root/'web-supervisor.sh').write_text('#!/bin/sh\necho web-manager >> \"$SLOT_EVENTS\"\n')
        (self.root/'web-supervisor.sh').chmod(0o755)
        shutil.copy2(self.binary,self.root/'zt30-slot')
        (self.root/'active').symlink_to('slots/'+'a'*64) # deliberately unavailable payload
        prefix=(ROOT/'packaging/zt30/app-init.sh').read_text().split('mkdir -p /mnt')[0]
        script=Path(self.temp.name)/'boot-prefix.sh';script.write_text(prefix)
        env=os.environ|{'PATH':str(fake)+':'+os.environ['PATH'],'APCG_ZT30_APP_DIR':str(self.root),
                       'APCG_ZT30_RUN_DIR':str(run),'SLOT_EVENTS':str(events)}
        proc=subprocess.run(['sh',str(script)],env=env,capture_output=True)
        self.assertEqual(proc.returncode,0)
        until=time.monotonic()+2
        while time.monotonic()<until and not events.exists():time.sleep(.02)
        self.assertIn('recovery',events.read_text())

    def test_supervisor_recovers_after_invalid_slot_is_repaired(self):
        slot=self.slot('a'*64)
        run=Path(self.temp.name)/'run';run.mkdir()
        fake=Path(self.temp.name)/'bin';fake.mkdir()
        (fake/'pidof').write_text('#!/bin/sh\nexit 1\n');(fake/'pidof').chmod(0o755)
        events=Path(self.temp.name)/'events'
        (slot/'camera-app').write_text('#!/bin/sh\necho app >> "$SLOT_EVENTS"\nsleep 0.3\n')
        (slot/'zt30-web').write_text('#!/bin/sh\nif [ "$1" = -p ]; then echo listener >> "$SLOT_EVENTS"; exit 0; fi\necho logger >> "$SLOT_EVENTS"\ncat >/dev/null\n')
        shutil.copy2(self.binary,self.root/'zt30-slot')
        (self.root/'active').symlink_to('slots/'+'b'*64)
        env=os.environ|{'PATH':str(fake)+':'+os.environ['PATH'],'APCG_ZT30_APP_DIR':str(self.root),
                       'APCG_ZT30_RUN_DIR':str(run),'SLOT_EVENTS':str(events)}
        with (run/'supervisor.log').open('w') as log:
            proc=subprocess.Popen(['sh',str(ROOT/'packaging/zt30/app-supervisor.sh')],env=env,
                                  stdout=log,stderr=log,start_new_session=True)
            try:
                until=time.monotonic()+3
                while time.monotonic()<until and 'retrying' not in (run/'supervisor.log').read_text():time.sleep(.02)
                self.assertIsNone(proc.poll())
                self.assertIn('retrying',(run/'supervisor.log').read_text())
                self.assertFalse(events.exists())
                self.call('activate','a'*64)
                until=time.monotonic()+8
                while time.monotonic()<until:
                    if events.exists() and 'logger' in events.read_text():break
                    time.sleep(.02)
                # Camera supervisor recovers app/logger; independent web test
                # below verifies listener recovery while that pipeline blocks.
                self.assertIn('app',events.read_text())
                self.assertIn('logger',events.read_text())
                self.assertIsNone(proc.poll())
            finally:
                os.killpg(proc.pid,15);proc.wait(timeout=5)

    def test_independent_web_respawn_with_camera_still_running(self):
        old=self.slot('a'*64);new=self.slot('b'*64)
        run=Path(self.temp.name)/'run';run.mkdir()
        events=Path(self.temp.name)/'events'
        shutil.copy2(self.binary,self.root/'zt30-slot')
        for slot in (old,new):shutil.copy2(self.listener,slot/'zt30-web')
        self.call('activate','a'*64)
        env=os.environ|{'APCG_ZT30_APP_DIR':str(self.root),'APCG_ZT30_RUN_DIR':str(run),'SLOT_EVENTS':str(events)}
        proc=subprocess.Popen(['sh',str(ROOT/'packaging/zt30/web-supervisor.sh')],env=env,start_new_session=True)
        # A real long-lived dummy camera proves web service recovery does not
        # need the camera pipeline to return; this process never loads SDKs.
        camera=subprocess.Popen(['sleep','40'])
        try:
            until=time.monotonic()+5
            while time.monotonic()<until and not events.exists():time.sleep(.02)
            first=events.read_text().splitlines()[0]
            self.assertIn(str(old/'webroot'),first);self.assertIn('fd9=-1',first)
            old_pid=int(first.split()[1])
            self.call('activate','b'*64)
            os.kill(old_pid,15)
            until=time.monotonic()+14
            while time.monotonic()<until:
                if len(events.read_text().splitlines())>=2:break
                time.sleep(.02)
            second=events.read_text().splitlines()[1]
            self.assertIn(str(new/'webroot'),second);self.assertIn('fd9=-1',second)
            self.assertNotEqual(first.split()[1],second.split()[1])
            self.assertIsNone(camera.poll())
            self.assertIsNone(proc.poll())
        finally:
            os.killpg(proc.pid,15);proc.wait(timeout=5)
            camera.terminate();camera.wait(timeout=5)

    def test_supervisor_pins_sdk_and_logger_then_selects_new_slot(self):
        old=self.slot('a'*64);new=self.slot('b'*64)
        run=Path(self.temp.name)/'run';run.mkdir()
        fake=Path(self.temp.name)/'bin';fake.mkdir()
        (fake/'pidof').write_text('#!/bin/sh\nexit 1\n');(fake/'pidof').chmod(0o755)
        events=Path(self.temp.name)/'events'
        for slot,id in ((old,'a'),(new,'b')):
            (slot/'camera-app').write_text('#!/bin/sh\nprintf "app '+id+' %s\\n" "$CAMERA_APP_ZT30_SDK" >> "$SLOT_EVENTS"\nsleep 0.3\n')
            (slot/'zt30-web').write_text('#!/bin/sh\n[ "$1" = -p ] && exit 0\nprintf "logger '+id+' %s\\n" "$CAMERA_APP_WEBROOT" >> "$SLOT_EVENTS"\ncat >/dev/null\n')
        shutil.copy2(self.binary,self.root/'zt30-slot')
        self.call('activate','a'*64)
        env=os.environ|{'PATH':str(fake)+':'+os.environ['PATH'],'APCG_ZT30_APP_DIR':str(self.root),
                       'APCG_ZT30_RUN_DIR':str(run),'SLOT_EVENTS':str(events)}
        proc=subprocess.Popen(['sh',str(ROOT/'packaging/zt30/app-supervisor.sh')],env=env,start_new_session=True)
        try:
            until=time.monotonic()+5
            while time.monotonic()<until and not events.exists():time.sleep(.02)
            self.assertTrue(events.exists())
            self.call('activate','b'*64)
            until=time.monotonic()+7
            while time.monotonic()<until:
                if 'logger b ' in events.read_text():break
                time.sleep(.02)
            lines=events.read_text().splitlines()
            self.assertIn('app a '+str(old/'encoder-sdk.so'),lines)
            self.assertIn('logger a '+str(old/'webroot'),lines)
            self.assertIn('app b '+str(new/'encoder-sdk.so'),lines)
            self.assertIn('logger b '+str(new/'webroot'),lines)
        finally:
            os.killpg(proc.pid,15);proc.wait(timeout=5)

if __name__=='__main__':unittest.main()
