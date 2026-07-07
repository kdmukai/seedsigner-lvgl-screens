# TODO: Re-enable the screenshot-diff CI job near production

**Status:** Deferred (disabled 2026-07-07, early-stage dev). Not a bug — a deliberate
CI cost trade-off. The job is dormant, not deleted.

## What was disabled

The `screenshot-diff` job in `.github/workflows/pages.yml`. On every PR it rebuilt the
**base ref's entire gallery** (language-pack build via Docker/GHCR + C++ screenshot-gen
build + full render) and compared it against the PR's gallery, publishing an advisory,
non-gating render-diff to the job summary + an artifact.

It was gated off with an inline guard:

```yaml
    if: false && github.event_name == 'pull_request'
```

so the job always shows as **skipped**. The job body and all its `scripts/ci/ci.sh`
helpers (`build-fontpacks`, `build-screenshots`, `generate-screenshots`,
`compare-screenshots`, `screenshot-diff-summary`) are untouched.

## Why

It was the slowest job by a wide margin — building the base gallery roughly **doubles**
the PR build time — and a render regression diff has little value while the screens are
churning heavily (large intentional visual changes every PR, so the diff is almost
always "everything changed" noise). The `build` job still builds the gallery + web
runner once per PR, and `main` still builds + deploys the Pages site normally.

## When to re-enable

When the screens stabilize / we approach production and render regressions start to
matter — i.e. when a base-vs-PR pixel diff would catch *unintended* changes rather than
just echoing intended ones. At that point the extra CI minutes buy real signal.

## How to re-enable

1. In `.github/workflows/pages.yml`, on the `screenshot-diff` job, delete the
   `false && ` guard so the condition reads:

   ```yaml
       if: github.event_name == 'pull_request'
   ```

2. Remove the "TEMPORARILY DISABLED" comment block above the job (and delete this file).
3. Open a throwaway PR and confirm: the job runs, produces the diff in the job summary
   plus the `screenshot-diff` artifact, and still holds **no** write token (the
   read-only / no-PR-comment security posture must stay intact).

## Related

- The re-enable steps are also summarized inline in `.github/workflows/pages.yml`
  (comment above the `screenshot-diff` job).
- Pairs with the workflow edit that disabled it — commit this note alongside that change.
