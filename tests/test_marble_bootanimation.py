"""Host-only behavior checks for the marble init-owned animation request."""
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
START = ROOT / 'devices/marble/runtime/boot/start-phase4-marble.sh'
INIT = ROOT / 'devices/marble/runtime/init/hmos-init-phase4.cfg'
BEGIN = '# BEGIN marble native boot animation\n'
END = '# END marble native boot animation\n'
GATE = 'marble.debug.start.bootanim'
WAKE = 'marble.debug.display.wake'


class MarbleBootAnimationTests(unittest.TestCase):
    def setUp(self):
        self.start = START.read_text(encoding='utf-8')
        self.block = BEGIN + self.start.split(BEGIN, 1)[1].split(END, 1)[0] + END

    def exercise(self, scenario='normal'):
        with tempfile.TemporaryDirectory(prefix='marble-bootanim-') as tmp:
            root = Path(tmp)
            inputs = {
                'compat/bin/bootanimation-launcher': 0o700,
                'compat/bin/bootanimation-real': 0o700,
                'compat/lib64/libhmos_boot_vulkan.so': 0o600,
                'sys_prod/etc/bootanimation/bootanimation_custom_config.json': 0o600,
                'sys_prod/etc/bootanimation/cust_bootvideo.mp4': 0o600,
            }
            for rel, mode in inputs.items():
                if scenario == 'missing-video' and rel.endswith('.mp4'):
                    continue
                if scenario == 'missing-config' and rel.endswith('.json'):
                    continue
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('synthetic input\n')
                path.chmod(mode)
            (root / 'dev').mkdir()
            if scenario == 'duplicate':
                (root / 'dev/marble-bootanim-attempt').mkdir()
            if scenario == 'skip':
                skip = root / 'data/local/tmp/marble-skip-stages'
                skip.parent.mkdir(parents=True)
                skip.write_text('wifi, bootanim\n')
            # Stub only the BusyBox applet dispatch used by this block. No real
            # chroot, init, timeouts, sockets or target executables are used.
            prepare = r'''set -eu
R=$1; scenario=$2; B=mock_busybox
started=false; awake=false; handoff=false
log() { printf 'LOG %s\n' "$*"; }
mock_busybox() {
 case "$1" in
  tr|mkdir) command "$@" ;;
  timeout)
   [ "$2 $3 $4 $5 $6 $7" = "-k 1 2 $B chroot $R" ] || exit 91
   shift 7
   case "$1" in
    /compat/bin/native_sa_check)
     [ "$#" = 2 ] || exit 92
     printf 'SA %s\n' "$2" >> "$R/effects"
     case "$scenario:$2" in no-media:3002|no-codec:3011|no-power:3301) return 1;; esac ;;
    /system/bin/param)
     case "$2:$3" in
      get:bootevent.boot.completed) [ "$scenario" != completed ] && echo false || echo true ;;
      get:bootevent.bootanimation.started) [ "$scenario" != native-started ] && echo false || echo true ;;
      get:bootevent.renderservice.ready) [ "$scenario" != no-render ] && echo true || echo false ;;
      get:bootevent.bootanimation.finished) printf '%s\n' "$handoff" ;;
      get:marble.debug.start.bootanim)
       if [ "$scenario:$handoff" = unknown-close:true ]; then return 1; fi
       [ "$scenario" != foreign ] && echo "$started" || echo true ;;
      get:marble.debug.display.wake) [ "$scenario" != preawake ] && echo "$awake" || echo true ;;
      set:marble.debug.start.bootanim|set:marble.debug.display.wake)
       printf 'SET %s %s\n' "$3" "$4" >> "$R/effects"
       if [ "$3" = marble.debug.display.wake ]; then
        awake=$4
        if [ "$scenario:$4" = wake-timeout:true ]; then return 124; fi
       else
        started=$4
        if [ "$scenario:$4" = start-timeout:true ]; then return 124; fi
        if [ "$scenario:$4" = stop-error:false ]; then return 1; fi
       fi ;;
      *) exit 93 ;;
     esac ;;
    *) exit 94 ;;
   esac ;;
  *) exit 95 ;;
 esac
}
'''
            run = subprocess.run(
                ['sh', '-c', prepare + self.block +
                 '\nbootanim_rearm_wake\nhandoff=true\nbootanim_finish\necho DESKTOP_PATH_REACHED\n',
                 'mock', str(root), scenario], capture_output=True, text=True, timeout=5, check=True,
            )
            self.assertIn('DESKTOP_PATH_REACHED', run.stdout)
            effects = root / 'effects'
            events = effects.read_text().splitlines() if effects.exists() else []
            return run.stdout, events

    def test_native_request_and_handoff_keep_original_init_owner(self):
        cfg = json.loads(INIT.read_text())
        service, = (s for s in cfg['services'] if s['name'] == 'bootanimation')
        self.assertEqual(service['path'], ['/compat/bin/bootanimation-launcher'])
        self.assertEqual((service['uid'], service['secon'], service['disabled']),
                         ('graphics', 'u:r:bootanimation:s0', 1))
        for value, verb in (('true', 'start'), ('false', 'stop')):
            job, = (j for j in cfg['jobs'] if j.get('condition') == f'{GATE}={value}')
            self.assertEqual(job['cmds'], [f'{verb} bootanimation'])
        out, events = self.exercise()
        self.assertIn('BOOTANIM_START_REQUESTED=1 pixels_unverified=1', out)
        self.assertIn('BOOTANIM_STOP_REQUESTED=1 native_finished=true', out)
        self.assertEqual(events, ['SA 10', 'SA 3002', 'SA 3011', 'SA 3301',
                                  f'SET {WAKE} true', f'SET {GATE} true',
                                  f'SET {WAKE} false', f'SET {GATE} false'])
        self.assertLess(self.start.index('wait_sa 3011'), self.start.index(BEGIN))
        self.assertLess(self.start.index(END), self.start.index('stage=wifi_netstack'))
        self.assertIn(f'stage=wake_unlock\nbootanim_rearm_wake\npset {WAKE} true', self.start)
        self.assertIn('log "BOOT_COMPLETED_EMITTED"\n$B sleep 10\nbootanim_finish\n\nstage=capture', self.start)
        self.assertNotIn('set bootevent.bootanimation.', self.block)

    def test_missing_prerequisites_duplicate_or_external_owner_do_not_start_or_stop(self):
        for scenario, why in (
            ('missing-video', 'missing_player_or_donor_asset'), ('missing-config', 'missing_player_or_donor_asset'),
            ('skip', 'requested'), ('completed', 'already_started_or_completed'),
            ('native-started', 'already_started_or_completed'), ('foreign', 'already_started_or_completed'),
            ('no-render', 'render_not_ready'), ('no-media', 'sa_not_ready id=3002'),
            ('no-codec', 'sa_not_ready id=3011'), ('no-power', 'sa_not_ready id=3301'),
            ('duplicate', 'attempt_already_owned'),
        ):
            with self.subTest(scenario=scenario):
                out, events = self.exercise(scenario)
                self.assertIn(f'BOOTANIM_SKIP={why}', out)
                self.assertFalse(any(e.startswith('SET ') for e in events))

    def test_wake_already_requested_is_not_rearmed(self):
        _, events = self.exercise('preawake')
        self.assertNotIn(f'SET {WAKE} false', events)
        self.assertIn(f'SET {GATE} true', events)
        self.assertIn(f'SET {GATE} false', events)

    def test_timeouts_do_not_hold_the_desktop_or_abandon_owned_cleanup(self):
        out, events = self.exercise('wake-timeout')
        self.assertIn('BOOTANIM_SKIP=early_wake_request_failed', out)
        self.assertIn(f'SET {WAKE} false', events)
        self.assertNotIn(f'SET {GATE} true', events)
        for scenario in ('start-timeout', 'unknown-close', 'stop-error'):
            with self.subTest(scenario=scenario):
                out, events = self.exercise(scenario)
                self.assertIn(f'SET {GATE} false', events)
                if scenario == 'start-timeout':
                    self.assertIn('BOOTANIM_START_REQUEST_FAILED', out)
                    self.assertNotIn('BOOTANIM_START_REQUESTED=1', out)
                if scenario == 'stop-error':
                    self.assertIn('BOOTANIM_STOP_REQUEST_FAILED', out)

    def test_h20_splice_does_not_replace_other_boot_stages(self):
        from tools.packaging.build_marble_h20_bootanimation import (
            AUDIO_TO_WIFI, HANDOFF, START_SHA, WAKE, splice_animation,
        )

        local = ROOT / 'work/theme-codec-now/H18-start-phase4-marble.sh'
        if local.is_file():
            baseline = local.read_bytes()
            self.assertEqual(hashlib.sha256(baseline).hexdigest(), START_SHA)
        else:
            baseline = (b'#!/bin/sh\n' + AUDIO_TO_WIFI + b'\n# untouched network setup\n' +
                        WAKE + b'# untouched desktop handoff\n' + HANDOFF)
        patched = splice_animation(baseline, self.start.encode())
        restored = patched.replace(self.block.encode() + b'\n', b'', 1)
        restored = restored.replace(b'\nbootanim_rearm_wake\n', b'\n', 1)
        restored = restored.replace(b'\nbootanim_finish\n', b'\n', 1)
        self.assertEqual(restored, baseline)
        subprocess.run(['sh', '-n'], input=patched, check=True)
        with self.assertRaisesRegex(ValueError, 'not unique'):
            splice_animation(baseline + AUDIO_TO_WIFI, self.start.encode())
        with self.assertRaisesRegex(ValueError, 'Missing animation handoff'):
            splice_animation(baseline, self.start.replace('\nbootanim_finish\n', '\n', 1).encode())

    def test_script_parses_in_both_shells(self):
        subprocess.run(['sh', '-n', str(START)], check=True)
        busybox = shutil.which('busybox')
        if busybox:
            subprocess.run([busybox, 'ash', '-n', str(START)], check=True)
