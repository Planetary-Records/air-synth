# Public contributor CI

The [workflow example](public-contributor-ci.yml.example) is inert outside .github/workflows. It is not a green fork result or permission to spend money.

Before activation, review the exact public repository's executor/settings, detach its development-PC runner access and verify it cannot inherit organization self-hosted runners. Use only an approved disposable GitHub-hosted runner within the public/free allowance. The template uses pull_request, read-only contents, pinned checkout without persisted credentials and no secrets/deployment/privileged handoff. See [CI safety](public-ci-safety.md) and [GitHub fork event behavior](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request).

The job builds the VST3 and core/state/render executables with probes off, then runs exactly three functional CTest entries. This is separate from the private release campaign, performance, DAW and listening acceptance. Public source intentionally omits private historical witness inputs and tools. Do not stage those into a fork to manufacture acceptance.

Observe an actual first-attempt fork run after setup, preserving exact head/workflow/image/compiler identity and failures. Until then public CI acceptance is open. [Building](building.md) gives the same functional path locally.
