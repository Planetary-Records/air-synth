import contextlib
import importlib.util
import io
from pathlib import Path
from urllib.parse import urlsplit, parse_qs
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ci_repeat_guard", ROOT / "tools" / "ci_repeat_guard.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
SHA = "a" * 40
FULL = "Windows native checks"

class RepeatGuardTests(unittest.TestCase):
    def run_main(self, fake, attempt=1):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(guard, "gh_json", side_effect=fake), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = guard.main(["--repository", "o/r", "--sha", SHA, "--run-id", "999", "--run-attempt", str(attempt)])
        return code, out.getvalue(), err.getvalue()

    @staticmethod
    def response(runs, jobs):
        def fake(path):
            url = urlsplit(path); params = parse_qs(url.query); page = int(params.get("page", ["1"])[0])
            if url.path.endswith('/runs'):
                subset = runs[(page-1)*100:page*100]
                return {"total_count": len(runs), "workflow_runs": subset}
            run_id = int(url.path.split('/')[-2]); entries = jobs[run_id]
            subset = entries[(page-1)*100:page*100]
            return {"total_count": len(entries), "jobs": subset}
        return fake

    def test_completed_full_including_cancelled_and_timed_out_are_prior(self):
        runs = [{"id": i, "conclusion": c, "jobs": [FULL]} for i,c in [(1,"failure"),(2,"success"),(3,"cancelled"),(4,"timed_out")]]
        runs += [{"id": 5,"conclusion":"success","jobs":["Focused native checks (not release acceptance)"]}]
        self.assertEqual(guard.prior_full_runs(runs, 999), [1,2,3,4])

    def test_refuses_earlier_cancelled_full_and_accepts_targeted_only(self):
        run = {"id":4,"status":"completed","conclusion":"cancelled"}
        code, _, err = self.run_main(self.response([run], {4:[{"id":40,"name":FULL}]}))
        self.assertEqual(code,1); self.assertIn("4",err)
        code, _, _ = self.run_main(self.response([run], {4:[{"id":40,"name":"Focused native checks (not release acceptance)"}]}))
        self.assertEqual(code,0)

    def test_extended_runs_are_counted_separately_from_the_gate(self):
        runs = [{"id": 1, "conclusion": "failure", "jobs": [guard.EXTENDED_JOB]},
                {"id": 2, "conclusion": "success", "jobs": [FULL]}]
        self.assertEqual(guard.prior_full_runs(runs, 999), [2])
        self.assertEqual(guard.prior_full_runs(runs, 999, guard.EXTENDED_JOB), [1])

    def test_unreadable_and_malformed_history_fail_closed(self):
        for fake in (lambda _: (_ for _ in ()).throw(guard.subprocess.CalledProcessError(1,['gh'])),
                     lambda _: {}, lambda _: {"total_count":1,"workflow_runs":[]}):
            with self.subTest(fake=fake):
                code, _, err = self.run_main(fake); self.assertNotEqual(code,0); self.assertIn("history",err.lower())

    def test_same_run_retry_is_rejected_without_user_override(self):
        fake = mock.Mock(side_effect=AssertionError('no API needed'))
        code, _, err = self.run_main(fake, attempt=2)
        self.assertNotEqual(code,0); self.assertIn('attempt',err.lower()); fake.assert_not_called()

    def test_paginates_runs_and_jobs_and_finds_last_page(self):
        runs = [{"id":i,"status":"completed","conclusion":"success"} for i in range(1,102)]
        jobs = {i:[{"id":i*1000,"name":"Measurement run (not release acceptance)"}] for i in range(1,102)}
        jobs[101]=[{"id":101000+j,"name":"other"} for j in range(100)]+[{"id":101100,"name":FULL}]
        code, _, err = self.run_main(self.response(runs,jobs));self.assertEqual(code,1);self.assertIn('101',err)

    def test_missing_or_inconsistent_job_history_fails_closed(self):
        run={"id":1,"status":"completed","conclusion":"success"}
        for jobs in ([],[{"id":1}],):
            with self.subTest(jobs=jobs):
                code, _, _=self.run_main(self.response([run],{1:jobs}));self.assertNotEqual(code,0)

    def test_changed_total_and_duplicate_job_ids_fail_closed(self):
        runs = [{"id": i, "status": "completed", "conclusion": "success"} for i in range(1, 102)]
        fake = self.response(runs, {i: [{"id": i, "name": "other"}] for i in range(1, 102)})
        def changed(path):
            result = fake(path)
            if "page=2" in path:
                result["total_count"] += 1
            return result
        code, _, _ = self.run_main(changed)
        self.assertNotEqual(code, 0)
        one = [{"id": 1, "status": "completed", "conclusion": "success"}]
        code, _, _ = self.run_main(self.response(one, {1: [{"id": 7, "name": "other"}, {"id": 7, "name": FULL}]}))
        self.assertNotEqual(code, 0)

    def test_workflow_supplies_the_run_attempt_and_explicit_override(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text(encoding="utf-8")
        self.assertIn("if: inputs.mode == 'full' && !inputs.rerun", workflow)
        self.assertIn('--run-attempt "$env:GITHUB_RUN_ATTEMPT"', workflow)

    def test_incomplete_or_duplicate_pagination_and_search_limit_fail_closed(self):
        for result in ({"total_count":101,"workflow_runs":[{"id":1,"status":"completed","conclusion":"success"}]},
                       {"total_count":1001,"workflow_runs":[]}):
            with self.subTest(result=result):
                code, _, _=self.run_main(lambda _: result);self.assertNotEqual(code,0)
        runs=[{"id":1,"status":"completed","conclusion":"success"}]*101
        code, _, _=self.run_main(self.response(runs,{1:[{"id":1,"name":"other"}]}));self.assertNotEqual(code,0)

if __name__ == '__main__':
    unittest.main()
