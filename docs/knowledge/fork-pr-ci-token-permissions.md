# Fork PRs get a read-only GITHUB_TOKEN — deploy/comment jobs fail with 403

## Symptom

On a PR opened **from a fork** into this repo, the build jobs pass but two
jobs fail with HTTP 403:

- `compare-and-deploy-pr` (Build & Deploy Screenshots) — `git push origin gh-pages`:
  `remote: Permission to <owner>/<repo>.git denied to github-actions[bot]` / `error: 403`
- `pr-comment` (Build Screen Runner) — `POST /repos/.../issues/{n}/comments`:
  `HttpError: Resource not accessible by integration` with
  `x-accepted-github-permissions: issues=write; pull_requests=write`

The same workflows succeed for **same-repo branch PRs**.

## Root cause

Both workflows trigger on `pull_request`. When that event originates from a
**fork**, GitHub forcibly downgrades `GITHUB_TOKEN` to **read-only** and
withholds secrets — *regardless* of the `permissions:` block in the workflow.
The declared `contents: write` / `pull-requests: write` only takes effect for
same-repo PRs. So any job that pushes (gh-pages) or comments (issues API)
cannot, and fails.

This is GitHub's security model: a fork's PR code is untrusted, so it must not
run against the base repo with a writable token or secrets.

This matters here because the normal contribution flow is fork→upstream
(`kdmukAI-bot` fork → `kdmukai` canonical), so *every* upstream PR hit this.

## Fix applied (PR #16)

Gate the privileged jobs so they only run for same-repo PRs, where the token is
writable. On fork PRs they are cleanly **skipped — not failed** — so CI goes
green while the build jobs still run and upload artifacts:

```yaml
if: github.event_name == 'pull_request' && github.event.pull_request.head.repo.fork == false
```

Applied to three jobs:
- `screenshots.yml`: `compare-and-deploy-pr` (gh-pages push + PR comment)
- `screenshots.yml`: `cleanup-pr-preview-pages` (gh-pages push on PR close — same 403)
- `screen-runner.yml`: `pr-comment` (PR comment)

To still validate the screenshot diff/preview and artifact comment, open a
**same-repo PR within the fork first** (branch → fork `main`, writable token),
review the output there, then open the fork→upstream PR which skips those steps.

This is the lightweight option. The heavier, fully-featured alternative — keep
preview/comment working on fork PRs via a two-stage `workflow_run` split — is
described in `.claude/plans/workflow-hardening.md`, which approaches the same
token mechanism from the *security* angle (same-repo branch PRs still get full
declared scope, which is a separate risk).

## Two non-obvious gotchas

1. **A workflow fix only takes effect once merged to the base branch.** For
   `pull_request` events the workflow definition is read from the **base** repo
   (`upstream/main`), not the PR head. So the existing failing PR won't go green
   from the fix until the fix is merged to upstream `main`.

2. **"Re-run all jobs" does NOT pick up the fix.** A re-run replays the workflow
   at the *original run's commit SHA/ref*, so it reads the **old** workflow
   files and fails identically. To get an existing PR onto the fixed workflows
   you must trigger a **fresh** run after the base is updated: close & reopen the
   PR (`reopened` event), or push a new commit to its head (`synchronize` event).
