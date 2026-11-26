from pathlib import Path
import importlib.util
import unittest
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('mic_modes',ROOT/'tools/maint/verify_stock_mic_modes.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class StockMicModeTests(unittest.TestCase):
    def test_low_latency_is_mono_not_voice_stereo(self):
        d=ROOT/'work/marble-mic-xml.ijm5fxrv'
        if not (d/'resourcemanager_ukee_mtp.xml').exists():self.skipTest('stock audio XML absent')
        r=m.describe((d/'resourcemanager_ukee_mtp.xml').read_bytes(),(d/'mixer_paths_overlay_static.xml').read_bytes())
        self.assertEqual(r['low_latency']['hardware_channels'],1)
        self.assertEqual(r['voip_tx']['hardware_channels'],2)
        self.assertTrue(r['no_voice_usecase_substitution'])
    def test_capture_frames_use_negotiated_channel_count(self):
        source=(ROOT/'compat/audio/pal_capture_backend.inc').read_text()
        self.assertIn('channels!=1 && channels!=2',source)
        self.assertIn('c->capture_channels=channels',source)
        self.assertIn('frame_bytes=c->capture_channels*2',source)
        self.assertIn('received/frame_bytes',source)
        self.assertNotIn('c->frames+=(unsigned)received/4',source)

    def test_deployment_candidate_has_exact_stripped_baseline(self):
        import json
        p=ROOT/'work/pal-mono-strip-audit.jYqNjq/report.json'
        if not p.exists():self.skipTest('strip identity audit absent')
        r=json.loads(p.read_text())
        self.assertTrue(r['allocated_sections_identical'])
        self.assertTrue(r['baseline_after_llvm_strip_exact_byte_match'])
        self.assertEqual(r['installed_baseline_sha256'],
                         '054b15307451eff35a1c96bcbbd7952401475b23b2fce3ca842795d64e033100')

    def test_vdi_reports_mono_frame_size(self):
        v=(ROOT/'compat/audio/audio_capture_vdi.inc').read_text()
        self.assertIn('attrs->channelCount!=1',v)
        self.assertIn('HMOS_PAL_CAPTURE_OPEN,48000,1',v)
        self.assertIn('AUDIO_VDI_CHANNEL_MONO; cap->channelCount=1',v)
        self.assertIn('*size=2; return 0;',v)
        client=(ROOT/'compat/audio/pal_client.c').read_text()
        self.assertIn('(capacity&1)',client)
        self.assertIn('(packet.reply.bytes&1)',client)

    def test_unknown_xml_rejected(self):
        with self.assertRaises(ValueError):m.describe(b'<audio/>',b'<mixer/>')
