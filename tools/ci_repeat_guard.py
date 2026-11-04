"""Refuse a second full Windows gate (or extended run) for a commit that already finished one."""
import argparse
import json
import subprocess
import sys

FULL_JOB = "Windows native checks"
EXTENDED_JOB = "Windows native extended checks"
PAGE_SIZE = 100
# GitHub caps filtered workflow-run search at 1,000 results. At the cap we cannot
# prove there is no omitted run, even when total_count itself is capped.
SEARCH_LIMIT = 1000


def prior_full_runs(runs, current_run_id, job=FULL_JOB):
    return sorted(run["id"] for run in runs
                  if run["id"] != current_run_id and run.get("conclusion")
                  and job in run.get("jobs", ()))


def gh_json(path):
    result = subprocess.run(["gh", "api", path], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def pages(path, key, *, search_limited=False):
    items, seen = [], set()
    expected = None
    page = 1
    while True:
        separator = "&" if "?" in path else "?"
        result = gh_json(f"{path}{separator}per_page={PAGE_SIZE}&page={page}")
        total, chunk = result["total_count"], result[key]
        if not isinstance(total, int) or isinstance(total, bool) or total < 0 or not isinstance(chunk, list):
            raise ValueError("invalid pagination metadata")
        if (search_limited and total >= SEARCH_LIMIT) or total > SEARCH_LIMIT:
            raise ValueError("history reached the bounded 1000-result limit")
        if expected is None:
            expected = total
        if total != expected or len(chunk) != min(PAGE_SIZE, expected - len(items)):
            raise ValueError("incomplete or changing paginated history")
        for item in chunk:
            identifier = item["id"]
            if not isinstance(identifier, int) or isinstance(identifier, bool) or identifier <= 0 or identifier in seen:
                raise ValueError("invalid or duplicate history identity")
            seen.add(identifier)
            items.append(item)
        if len(items) == expected:
            return items
        page += 1


def finished_runs(repository, workflow, sha):
    runs = pages(f"repos/{repository}/actions/workflows/{workflow}/runs"
                 f"?head_sha={sha}&status=completed", "workflow_runs", search_limited=True)
    history = []
    for run in runs:
        if run.get("status") != "completed" or not isinstance(run.get("conclusion"), str) or not run["conclusion"]:
            raise ValueError("completed run has incomplete status or conclusion")
        jobs = pages(f"repos/{repository}/actions/runs/{run['id']}/jobs?filter=all", "jobs")
        if not jobs or any(not isinstance(job.get("name"), str) or not job["name"] for job in jobs):
            raise ValueError("completed run has incomplete job history")
        history.append({"id": run["id"], "conclusion": run["conclusion"], "jobs": [job["name"] for job in jobs]})
    return history


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--workflow", default="windows-native.yml")
    parser.add_argument("--sha", required=True)
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--run-attempt", type=int, required=True)
    # Each tier gets one finished run per commit; the gate and the extended run are counted separately.
    parser.add_argument("--job", choices=(FULL_JOB, EXTENDED_JOB), default=FULL_JOB)
    args = parser.parse_args(argv)
    if args.run_id <= 0 or args.run_attempt <= 0:
        print("REPEAT_GUARD invalid run identity or attempt", file=sys.stderr)
        return 1
    # A re-run retains its run ID and can hide its previous attempt from a
    # completed-runs query while it is in progress. Only the workflow's explicit
    # infrastructure rerun input may bypass this guard.
    if args.run_attempt != 1:
        print("REPEAT_GUARD refused: repeated GitHub run attempt; use the explicit infrastructure rerun override only when authorized", file=sys.stderr)
        return 1
    try:
        runs = finished_runs(args.repository, args.workflow, args.sha)
        prior = prior_full_runs(runs, args.run_id, args.job)
    except (OSError, subprocess.CalledProcessError, KeyError, ValueError, TypeError) as error:
        print(f"REPEAT_GUARD could not verify complete run history ({error}); refusing this gate", file=sys.stderr)
        return 1
    if prior:
        print(f"REPEAT_GUARD refused: commit {args.sha} already finished '{args.job}' run(s) "
              f"{', '.join(map(str, prior))}. Fix locally and push a new commit, or re-dispatch with "
              "rerun=true only for a runner or infrastructure fault.", file=sys.stderr)
        return 1
    print(f"REPEAT_GUARD first '{args.job}' run for {args.sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
