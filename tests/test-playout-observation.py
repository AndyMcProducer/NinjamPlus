import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('playout', Path(__file__).with_name('analyze-playout-observation.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PlayoutEvidenceTests(unittest.TestCase):
    def test_matches_frame_timestamp_and_counts_compositor_delay(self):
        observation = {'snapshots': [{'peer': 'p', 'label': 'alice'}],
                       'decoded': [{'peer': 'p', 'at': 1950, 'timestamp': 1234567, 'perf': 100, 'estimatedCaptureAgeMs': 7950},
                                   {'peer': 'p', 'at': 9000, 'timestamp': 1234567, 'perf': 9000, 'estimatedCaptureAgeMs': 100}],
                       'presented': [{'peer': 'p', 'at': 2000, 'mediaTime': 1.234567,
                                      'expectedDisplayTime': 150}]}
        native = [{'at': 1900, 'intervals': {'value': [
            {'type': 'intervalInfo', 'wallClockMs': 1900, 'bpm': 120, 'bpi': 16},
            {'type': 'videoTimecode', 'userKey': 'alice', 'bufferTotalMs': 8000}]}}]
        result = module.analyze(observation, native)['alice']
        self.assertEqual(result['displayedFrameAgeMs']['median'], 8000)
        self.assertEqual(result['decoderOutputToDisplayMs']['median'], 50)
        self.assertEqual(result['displayAgeMinusNativeTargetMs']['median'], 0)
        # A stale native snapshot cannot be reported as current sync evidence.
        observation['presented'][0]['at'] = 5000
        self.assertIsNone(module.analyze(observation, native)['alice']['displayAgeMinusNativeTargetMs'])

    def test_unmatched_display_frame_is_not_assumed_in_sync(self):
        observation = {'snapshots': [{'peer': 'p', 'label': 'alice'}], 'decoded': [],
                       'presented': [{'peer': 'p', 'at': 1000, 'mediaTime': 1, 'expectedDisplayTime': 1},
                                     {'peer': 'p', 'at': 9000, 'mediaTime': 2, 'expectedDisplayTime': 8001}]}
        result = module.analyze(observation, [])['alice']
        self.assertEqual(result['matchedDecodedFrames'], 0)
        self.assertIsNone(result['displayedFrameAgeMs'])
        self.assertEqual(result['displayGapMs']['max'], 8000)


if __name__ == '__main__':
    unittest.main()
