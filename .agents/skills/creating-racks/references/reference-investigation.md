# Understanding the reference plugin

When the user names a plugin or product for a rack, understanding how that plugin works comes before anything else: before the design, the maker, the brief, a drawing or a spec. Understand it from the user's installed copy, show the user the evidence, and only then build a similar rack. A rack built from scratch skips this.

Do not ask the user whether to investigate, how deeply, or which evidence to collect: collect all of it, as below. **REA MCP is mandatory whenever investigating another tool**, including another synth, plugin or VST3. The Windows direct stdio fallback in "With REA MCP" is already authorized and satisfies this requirement. Further setup or proceeding-without-MCP questions apply only as described there. If the plugin cannot be found, loaded or rendered (missing, unlicensed, crashing headless), stop and tell the user exactly what failed. For REA MCP failures, try direct MCP before the repair and stop-and-ask gate below. Do not silently fall back on the manual, measurements or ordinary REA CLI analysis.

The rack itself stays original:
- **original DSP** in C++20/JUCE, on the native modules or a new one. The plugin's measured behaviour is a target to design to; no code, coefficients, tables, presets or assets are copied from it;
- **a fictional maker** with its own name, era and look (rule 4 of `SKILL.md`), which differs from the plugin's own interface and branding as much as from the three makers;
- **original names**: the plugin is a behaviour reference, named in the investigation record and the spec. The rack's name, maker, legends and intent are original, and the concept prompts forbid real brand names and logos.

Work only on the user's installed, licensed copy, and never bypass its copy protection.

## The questions

Write the design questions the rack needs answered: what each control does, over what range and with what curve; where each process sits in the signal path (on the wet signal, inside a feedback loop, on the output); which modes exist and how they change the controls; what the face reaches without opening anything; what readouts it shows and from what data. Then read the existing modules' parameters in `src/state/ParameterRegistry.cpp` and their DSP, so each answer can be compared with what the synth already does.

## The evidence, all of it

Every question is answered from the plugin itself. Use every source below that bears on a question; do not stop at the first one that seems to answer it.

1. **Identity.** Find the plugin in the user's VST3 folder (`C:\Program Files\Common Files\VST3` by default). Record its path, SHA256, size, architecture and file version before anything else, and check the SHA256 again at the end.
2. **The host's parameter list.** Load the plugin in a headless host: JUCE's `VST3PluginFormat`, as `tools/native/RenderFixtures.cpp` loads the synth, or a development-tooling host under `tools/`. Record every parameter: its name, ID, default, step count, units, and its display text at sampled normalised values (every twentieth of the range, plus every step of a stepped parameter). These fix the ranges, defaults and curves. Record the latency, tail, buses and factory program names too.
3. **Measurement.** Render test signals offline through the installed copy, at a fixed sample rate and block size, moving one parameter at a time from a neutral setting:
   - a nonlinearity: sine level steps, for the input-to-output curve, the harmonics and the output level;
   - a filter: impulses and sine sweeps, for the magnitude response and slope at several settings, and, in a feedback loop, the change from one repeat to the next;
   - modulation: a sustained sine, for the pitch or time deviation against depth and rate;
   - delays and reverbs: impulses, for the timing and the decay of each repeat;
   - every render: latency, level and stereo behaviour.

   Keep the raw renders, the analysis script and its plots ("Record it" below). The host and the analysis are development tooling, never shipped.
4. **Mandatory REA MCP investigation** of the installed binary: use focused static analysis to investigate the parameter tables, display formatting, mode lists, constants and signal paths relevant to the rack. REA MCP is required even when the parameter list and measurements appear sufficient. See "With REA MCP" below; runtime claims still require runtime evidence.
5. **The official manual and documentation** name the controls and state the intent. Use them to choose the questions and to cross-check the evidence above. A claim taken only from the manual is marked "documented, not observed", and it never settles a range, curve or law that the plugin can be loaded or measured for.

## Show the user the evidence

Before composing the design, show the user what was found. For each question, give the answer, the evidence behind it (plots as inline images; parameter ranges and display values in short tables; numbers with their units) and its source: the parameter dump, the measurement file, the REA Evidence ID or the manual page. Add what stays unknown and why, and how each capability maps onto the synth (below). Then go on to compose the design without waiting for permission to have investigated. Choices that change the product's scope, such as new DSP, are still the user's.

## With REA MCP

Follow the `reverse-engineer-anything` skill's mandatory MCP policy. Apply this gate before substantive reference-product investigation or designing anything from it; read-only target discovery and connection diagnostics may identify what to connect:

1. Discover the REA MCP tools and use the connected server's actual schemas. A successful target-relevant MCP call is required; an installed skill, tool listing, doctor check or ordinary CLI analysis result alone does not satisfy it.
2. If the client tools are absent, broken or blocked, launch `cmd.exe /d /c npx -y rea-agents@6.7.0 mcp` on Windows and use an MCP stdio client to communicate with that persistent owned process. This exact fallback, including package acquisition, was authorized by the user on 2026-10-22; do not ask again to launch it. Complete `initialize` and `notifications/initialized`, discover schemas with `tools/list`, then use `tools/call`. Keep the same server for stateful operations, retain its Evidence IDs/exports, close the owned binary session and shut down only the process this task started. Successful target-relevant calls are MCP evidence and need no exception. Startup alone is insufficient. Follow the `reverse-engineer-anything` connection-and-recovery guide for details and honor host execution/access policies.
3. If both routes fail, you **MUST attempt connection repair**: inspect client and direct-server errors, registration, launcher/runtime, environment and timeouts; correct the observed cause within authorization and retry an actual MCP call through either connection. An installed `rea doctor --client <client> --json` helps diagnose registration; it does not prove an active connection. For setup beyond the authorized pinned launch, check the official [REA GitHub repository](https://github.com/morluto/rea) and [installation instructions](https://github.com/morluto/rea/blob/main/docs/installation.md), show paths, backups and changes and obtain any missing approval. Preserve the local policy with `--skill=false` unless skill replacement is approved. Do not weaken client permissions or repeatedly reinstall. A client restart need not block a working direct connection. Resolve required provider failures too; metadata-only success does not prove decompilation is available.
4. If neither MCP route can be used after repair, **stop the investigation and dependent design/implementation**. Warn explicitly: **"I was not able to use REA MCP."** State both connection failures, attempted repairs and remaining blocker. Explain the limitations for this rack: host dumps establish exposed controls, renders establish only the tested responses, and manuals describe intended behavior; without the missing MCP analysis, internal routing, algorithms, constants and untested behavior may remain unknown. An ordinary CLI analysis result is not MCP evidence, and equivalent coverage or fidelity cannot be claimed. Ask whether to fix REA MCP first (recommended) or continue without it (**not recommended**). Wait for explicit user approval before investigation without MCP or dependent work; general implementation approval or silence is not that exception. Declining client registration does not prevent using the authorized direct MCP fallback.
5. Only after that explicit exception may the agreed alternate evidence be used. Record the user's approval, missing MCP evidence, limitations and unknowns, and show them with the findings. Never mark MCP investigation as completed. The exception concerns MCP only; the installed-copy, host-measurement and original-work requirements still apply.

For a working connection:
- A Windows VST3's native module is a PE: use MCP `open_binary`, then focused searches for the strings, parameter names and functions behind the design questions. Do not set out to reverse engineer the whole plugin. Close the owned session (`close_binary`).
- Keep observations (what the evidence shows, with Evidence IDs), inferences (what you conclude, and from which observations) and unknowns apart, and never present static analysis as observed execution.

The REA-led racks' investigations are the worked examples of REA on a plugin: `docs/verification/2026-10-17-pro-q4-investigation.json` (Pro-Q 4), `docs/verification/2026-10-18-saturation-reference-investigation.json` with the Decapitator passes beside it, and `docs/design/audio/2026-10-17-pro-r-discovery.md` on the `codex/pro-r-reverb` branch (Pro-R). Reuse their findings for the same plugin. Only the investigations are examples: the racks they fed are not models for a face, a binding or the AI's reach, which follow the merged makers. `docs/verification/2026-10-18-echoboy-investigation.json`'s first version is the counter-example: it answered from the manual alone, without loading, measuring or analysing the installed plugin.

## Map it onto the synth

For each capability the rack needs, record one of:
- **an existing module and keys**, which the rack composes with no DSP change;
- **a new module or new keys**, a separate DSP feature with its own spec, plan and user approval (see "A new module" in [skin-and-integration.md](skin-and-integration.md)). Its ranges, defaults and laws come from the measured evidence, converted to the synth's conventions and cited, not chosen by reasoning;
- **left out**, with the reason.

Displays stay truthful: a meter, spectrum, curve or gain-reduction readout appears only where the synth computes that data for the rack; find its source in `src/` before drawing it. The merged makers draw none: they read values from engraved scales, the host's touch readout, and Glass Current's VFD, which the native renderer letters with real values. If no telemetry exists, leave the display out and do not draw it on the faceplate either. A reference plugin's graph is not a reason to put one on the face.

Respect the module limits: one voice per synth rack and one effect per FX rack for new designs, at most 8 focused controls, and the eight physical FX slots the synth has.

## Record it

Write `docs/verification/<date>-<product>-investigation.json` before the design, with:
- `question` and the design questions, each marked answered, partially answered or open, with the evidence that answered it;
- `target`: path, SHA256, bytes, architecture and file version;
- `host_parameter_list`: the host used and the dump's file and hash;
- `measurements`: each render's settings, signal, file and hash, the analysis script and its plots;
- `rea`: transport (`mcp`, or an explicitly approved alternative), connection (`client` or `direct-stdio` for MCP), version, provider, operations and Evidence IDs; for direct MCP, the exact launch command and original client failure; if neither MCP route worked, both errors, attempted repairs, missing evidence, limitations and the user's explicit dated exception (never a passing MCP result);
- `observations`, `inferences`, `official_documentation` and `unknowns`;
- `project_reuse`: the modules and keys it reuses, and the new work it needs;
- `user_scope`: what the user chose, quoted with its date;
- `checks`: the target's hash rechecked, and anything not run, with the reason.

Commit the parameter dump, the analysis script and the plots under `docs/verification/<date>-<product>-investigation/`. Keep the raw renders and REA exports in `build/<product>-investigation-<date>/`, outside Git, with their hashes in the record. The rack's brief and spec cite the record.
