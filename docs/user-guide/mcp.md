# Local agent access through MCP

AIR can expose each loaded instrument to a local MCP client. The companion executable supplies a native stdio bridge; the instrument keeps its normal offline and manual operation. External editing does not require AIR provider sign-in, a selected model, or an inference request.

For a first connection, use the [illustrated producer manual](https://air.planetary.music/manual#mcp). It includes a message you can paste into a local assistant to register air, discover a loaded instrument and prepare an edit for review. The built-in GPT feature's paid ChatGPT setup is separate; MCP itself does not require that subscription, while your chosen assistant has its own access and usage terms. A web chat without local tool access cannot reach this Windows stdio bridge.

## Set up your client

The plugin shows no setup text; configure your MCP client once from this guide. The bridge is the companion executable beside the plugin module. For a bundle in the installer's default location, add this table to your client's MCP configuration and reconnect that client:

```toml
[mcp_servers.air]
command = "C:\\Program Files\\Common Files\\VST3\\AI Rack Synth.vst3\\Contents\\x86_64-win\\AI Rack Synth Companion.exe"
args = ["--mcp-stdio"]
```

If you copied the bundle to another VST3 folder, use the absolute path of `Contents\x86_64-win\AI Rack Synth Companion.exe` inside it. A TOML basic string needs each backslash doubled and each quote escaped; a single-quoted literal string (`'C:\...\AI Rack Synth Companion.exe'`) takes the path as written. Keep the companion beside the VST3 binary as supplied by the installer. AIR does not write client settings, and the configuration holds no credentials or OAuth tokens. In Codex, the table belongs in its MCP configuration; see the [official MCP setup documentation](https://developers.openai.com/codex/mcp).

## The MCP light

The AI unit's bottom row has an **MCP** light beside LINKED INSTANCES. It is lit while agents can reach this loaded instance. Click it, or Tab to it and press Space, to show the MCP page on the AI screen: whether access is on, the start of the live instance ID with the observed client and request counts, and the most recent successful external Apply. Screen readers also hear the full instance ID, the sound label and that Apply's diff. Click the light again, or press Escape, to go back.

The first verified client is the installed Codex app-server. Protocol support is **MCP 2025-11-25 over stdio**, including tools and preview-resource reads. This release does not claim the different 2026-07-28 revision. Other clients must negotiate the supported revision; their permission prompts remain their policy. This is same-user Windows local IPC with remote access disabled. It opens no HTTP listener and offers no general filesystem or shell tool.

## Choose the instrument explicitly

Connect once, then call `list_instances`. The same connection works with no instruments loaded and can discover later instances. Pass the chosen `instance_id` with every targeted tool. Addresses identify loaded runtime incarnations; sound names and host track/project labels are display information. Host metadata may be unavailable. Saving a sound does not save its live address or client authority.

For a bridge restricted to one current address, the optional arguments are `args = ["--mcp-stdio", "--instance", "<live-instance-id>"]`. A pinned bridge rejects other targets. A reload or duplication requires discovery again; do not reuse old snapshots, edits, jobs, or preview URIs after a control-epoch change.

Leaving the MCP page or closing the AI surface or plugin editor leaves the processor endpoint available. **DISABLE ACCESS** on the MCP page revokes this loaded instance's external work and rejects new external commands. The committed sound remains available to the host and manual controls. This switch is runtime-local. **ENABLE ACCESS** starts a fresh epoch; agents must reread and prepare again.

The client count means connected local clients **across AIR**, including clients that have only read data. It is not a per-instance agent count or an edit reservation. Activity counts requests admitted for this instance, including reads; it does not claim native execution or inference. An unavailable summary stays labelled unavailable. The latest-diff display describes the most recent external Apply; subsequent manual or history operations can change the current sound.

## Inspect, prepare, and Apply

1. Read `get_state` and `get_catalog`. Catalogs are paged and bind a snapshot. Definitions are retrieved using their captured identity, revision, and content hash. Missing host timing remains unavailable. Linked-peer context appears only after the user has explicitly admitted and selected peers through AIR's existing group workflow. MCP reads do not grant group admission.
2. Call `prepare_edit` with a current snapshot, policy, and typed parameter, macro, pattern, or library-rack edits. Read `get_edit` to inspect the returned diff. Preparation does not change committed sound. Locked, inactive, unreachable, unsupported, or stale controls are refused.
3. Optionally render or audition a preview. Rendering is not required for Apply.
4. Explicitly call `apply_edit`. AIR performs the normal native validation and host notifications, and adds one existing native undo action. There is no additional plugin-side confirmation click and no provider request.

Only one external staged edit or preview owner can mutate an instance at a time. Other clients may read. Competing mutable work returns `busy`; built-in AI mutable work and external staging respect each other's ownership. Manual/host edits remain responsive and can make a stage stale. On `stale`, reread state and prepare a new edit. On `busy`, finish or discard the owning work, then retry deliberately. The safe-dispatch deadline is 2,000 ms, active-frame deadline 5,000 ms, and owned handles expire after 300,000 ms of inactivity. Cancellation, disconnect, unload, revocation, or epoch loss also retires authority. No uncertain mutation is automatically replayed.

A retained repeated Apply returns its original receipt without another edit or undo action. `undo_edit` and `redo_edit` require that transaction ID and the current expected revision. They refuse unrelated user/client history. An expired or evicted receipt is not permission to repeat the edit.

## Preview and audio availability

`render_preview` renders an immutable committed state or the caller's staged candidate off the audio thread. `get_job` reports its actual state, WAV URI, and measurements. Previews use 48 kHz stereo float audio, at most eight seconds of body plus four seconds of tail (576,000 frames; maximum WAV 4,608,044 bytes). The resource contains only that generated preview; arbitrary files and original sample collections are unavailable. Owned local drum assets remain owned through rendering and recall.

Read a completed WAV through MCP `resources/read`. Use `play_preview` and `stop_preview` to audition through the existing plugin route. Playback needs an actively processing host; while suspended or after audio availability expires it returns `audio_unavailable`. AIR does not start the DAW or use a separate audio device. Stop remains available while jobs are busy. Apply waits for normal preview-stop acknowledgment. `cancel_job` and `discard_edit` retire caller-owned work. Preview failure preserves committed sound and does not silently Apply an edit. Measurements describe numerical audio; they are not listening acceptance.

## Availability and limits

The separate MCP domain supports 16 endpoints and eight local clients without consuming the existing provider/group connection capacity. Control requests are bounded to 1 MiB and results to 8 MiB. Catalogs paginate; preview WAVs use opaque resources. Queue, preview, catalog, and owned-memory capacity refusals leave committed sound intact. Read-only access does not reserve a mutable owner.

Helper loss invalidates external handles and reconnect establishes a fresh epoch. Manual audio and sound recall remain independent. A rare inherited Windows transport condition has a stronger limitation: if cancelled kernel I/O is still unobserved after 250 ms, its storage enters the existing fixed quarantine (at most 34 native owners), and that process refuses new native pipe read/write/connect admission. Already-admitted retirement may finish. Restarting only the helper cannot recover a poisoned DAW client; reload/restart the owning process before reconnecting. Ordinary cleanup tests do not prove this exceptional storage is released. AIR does not reset poison or trade callback safety for recovery.

The normal native callback adds no MCP parsing, IPC, file access, provider call, wait, mutex, or heap operation. Functional tests, numerical audio comparisons, installation, CI, DAW compatibility, and human listening are separate evidence scopes. Optional DAW sessions do not gate local development.
