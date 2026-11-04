# Native work without disrupting other sessions

Use a separate checkout and private build/output directories. Reuse its build tree for later edits. Serialize writers to one build tree and do not rebuild an executable while a test or app uses it.

Bootstrap tools/native-run once. Run subsequent native configure/build/direct test/render/validation commands through RackSynthNativeRun. Invoke CTest directly because it already coordinates individual tests. Only one desktop check runs at a time; automated windows use a private desktop. Do not move the physical pointer, change focus, close a musician's apps or play through speakers as part of an automated check.

Ordinary functional work can overlap in separate trees. Only timing/capacity measurements require an exclusive clean measurement. Keep the original workload and budget; report interference and failures rather than retrying until a pass.

A reviewed final release has a separate qualification gate. Contributor smoke is intentionally smaller; do not use an expensive full campaign to discover routine failures while iterating. See [building](building.md) and [CI safety](public-ci-safety.md).
