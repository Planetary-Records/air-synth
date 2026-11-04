# Public CI safety

Unknown pull-request code must never execute on a maintainer's development PC or inherit its credentials, local services or private folders. Detach any repository self-hosted runner and verify the public repository cannot inherit an organization runner group before publication. An offline runner or a workflow condition is not sufficient isolation.

Use a reviewed disposable hosted executor, ordinary pull_request events, read-only contents permission and checkout without persisted credentials. Do not combine untrusted checkout with privileged pull_request_target/workflow_run handoffs. Keep secrets, deployment and publication out of fork checks. See [GitHub's security guidance](https://docs.github.com/en/actions/reference/security/secure-use).

The [contributor workflow template](public-contributor-ci.md) is inert outside active workflows until the exact public setup is approved and a real fork run is observed. Paid execution is not authorized by this template. Static review and contributor evidence do not replace the release's final qualification.
