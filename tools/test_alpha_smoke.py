import copy
import unittest
from pathlib import Path
import alpha_smoke


def receipt(seconds=3600):
    return dict(success=True, simultaneous_instances=10, requested_soak_seconds=seconds,
                soak_elapsed_seconds=seconds + .01, full_hour_soak=seconds == 3600,
                soak_sample_rate=48000, rendered_frames_per_instance=seconds*48000,
                rate_buffer_rows=[dict(sample_rate=r, buffer=b, partition_difference=0,
                                       offline_realtime_difference=0)
                                  for r in (44100, 48000, 96000) for b in (64, 128, 256, 512, 1024)])


class Qualification(unittest.TestCase):
    def test_full_and_short_scopes_are_distinct(self):
        self.assertTrue(alpha_smoke.render_scope(receipt(), 3600))
        self.assertFalse(alpha_smoke.render_scope(receipt(1), 1))

    def test_short_or_stale_receipt_cannot_qualify_hour(self):
        for report in (receipt(1), dict(receipt(), soak_elapsed_seconds=1),
                       dict(receipt(1), full_hour_soak=True),
                       dict(receipt(), rendered_frames_per_instance=48000)):
            with self.subTest(report=report), self.assertRaises(ValueError):
                alpha_smoke.render_scope(report, 3600)

    def test_missing_duplicate_or_failing_audio_rows_refused(self):
        for change in ('missing', 'duplicate', 'nan', 'mismatch'):
            report=copy.deepcopy(receipt()); rows=report['rate_buffer_rows']
            if change=='missing': rows.pop()
            elif change=='duplicate': rows[-1]=rows[0]
            elif change=='nan': rows[0]['partition_difference']=float('nan')
            else: rows[0]['offline_realtime_difference']=.001
            with self.subTest(change=change), self.assertRaises(ValueError):
                alpha_smoke.render_scope(report, 3600)

    def test_no_overwrite_or_workspace_escape(self):
        for path in (alpha_smoke.ROOT/'build', alpha_smoke.ROOT/'README.md',
                     alpha_smoke.ROOT/'build/../../outside-smoke'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                alpha_smoke.fresh_output(Path(path))


if __name__ == '__main__':
    unittest.main()
