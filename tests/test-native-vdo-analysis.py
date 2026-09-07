"""Guard against treating frozen pictures as successful sync observations."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

class ComparisonTests(unittest.TestCase):
    def test_frozen_frame_is_excluded_but_advancing_interval_lead_is_measured(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for receiver, source in [('alpha', 'bravo'), ('bravo', 'alpha')]:
                # First pair is the same source frame; last frame advances but
                # is 5000 ms early compared with independently measured audio.
                samples = [{'now':10000,'delay':5000}, {'now':10200,'delay':5200},
                           {'now':10400,'delay':1000}]
                (root/(receiver+'-0.json')).write_text(json.dumps({'samples':samples,
                    'source':[{'at':10200,'captureAgeMs':20},{'at':10400,'captureAgeMs':20}]}))
                (root/f'live-audio-analysis-{source}-to-{receiver}.json').write_text(json.dumps([
                    {'at':10200,'audioDelayMs':5200,'correlation':.9},
                    {'at':10400,'audioDelayMs':6000,'correlation':.9}]))
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('analyze-native-vdo.py')),
                                     '--root', directory], capture_output=True, text=True, check=True)
            for direction in json.loads(result.stdout).values():
                self.assertEqual(direction['advancingFreshSourceWindows'], 1)
                self.assertFalse(direction['rows'][0]['advancing'])
                self.assertEqual(direction['medianVideoMinusAudioMs'], -5000)

if __name__ == '__main__':
    unittest.main()
