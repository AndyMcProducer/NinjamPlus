import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('timing', Path(__file__).with_name('analyze-presentation-timing.py'))
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)

class PresentationIdentityTest(unittest.TestCase):
    def fixture(self):
        return {'observer': {'decoded': [dict(peer='p', at=1000, perf=100, timestamp=5000000, frameIdentity=123)],
                             'presented': [dict(peer='p', at=1012, perf=112, expectedDisplayTime=116,
                                                mediaTime=1, frameIdentity=123)]}}

    def test_timestamp_rebase_does_not_change_identity(self):
        result = timing.analyze(self.fixture())
        self.assertEqual(result['matchedFrames'], 1)
        self.assertEqual(result['writerToCallbackWallMs']['median'], 12)
        self.assertEqual(result['writerToExpectedDisplayMs']['median'], 16)
        self.assertEqual(result['presentationMinusWriterTimestampMs']['median'], -4000)

    def test_same_timestamp_is_not_proof_of_same_frame(self):
        data = self.fixture()
        data['observer']['presented'][0].update(mediaTime=5, frameIdentity=124)
        self.assertEqual(timing.analyze(data)['matchedFrames'], 0)

    def test_duplicate_content_is_ambiguous(self):
        data = self.fixture()
        data['observer']['decoded'].append(dict(data['observer']['decoded'][0], at=1001))
        result = timing.analyze(data)
        self.assertEqual(result['matchedFrames'], 0)
        self.assertEqual(result['ambiguousFrames'], 1)

    def test_unmarked_video_remains_unverified(self):
        data = self.fixture()
        for frames in data['observer'].values():
            for frame in frames:
                frame.pop('frameIdentity')
        self.assertFalse(timing.analyze(data)['matchingComplete'])

if __name__ == '__main__':
    unittest.main()
