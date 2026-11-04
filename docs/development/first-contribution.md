# Your first contribution

Choose a small documentation correction, original sound or reproducible bug. A first change can fit a short session; tool installation and a cold build can take longer than 30 minutes.

Fork the public repository and clone your fork using its Code button's URL. Create a focused branch with `git switch -c docs/first-contribution`. Read [CONTRIBUTING](../../CONTRIBUTING.md) and [AGENTS.md](../../AGENTS.md). Do not import private history or generated evidence.

For code, follow the [Windows build and three-test smoke path](building.md). It requires no system-wide install, account or DAW. Preserve the first failure for a behavior fix and run its focused regression after the smallest complete change. For documentation, check wording and local links; compilation is unnecessary. For a sound, include name, intent, source version, playback instructions and rights to every embedded sample.

Inspect git diff/status, commit only intended files and open a PR to main. Use the template: problem and resulting behavior, actual checks and limits, and material rights/privacy. The [conduct policy](../../CODE_OF_CONDUCT.md) applies. No public PR runs on the development PC; [CI safety](public-ci-safety.md) explains the boundary.

Contributor smoke is not release, performance, DAW or listening acceptance. Publication and installation over a working setup need their own release checks.
