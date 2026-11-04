# Native OAuth feasibility probe

Windows-only C++20/JUCE console proof for M0 F2.3. It uses the direct local-app OAuth contract in the route record. The synth plugin and its offline/manual operation are independent of this executable.

## Build and offline verification

Run from the repository root in PowerShell with the installed Visual Studio Build Tools CMake:

```powershell
$env:CMAKE_GENERATOR = 'Visual Studio 17 2022'
$cmake = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
& $cmake --preset windows-debug
& $cmake --build --preset windows-debug --target RackSynthOAuthProbe RackSynthOAuthProbeTests RackSynthProbeTests RackSynthProbe_VST3
& $ctest --preset windows-debug --output-on-failure
python -m unittest discover -s tests -p 'test_*.py' -v
```

`RackSynthOAuthProbeTests` uses synthetic tokens, a newly generated RSA signing key, temporary DPAPI records, and a real IPv4 loopback socket. It never accesses the real credential directory or makes a live network request. A child copy of the test executable verifies exclusive file locking across processes.

## Human-run account proof, after review

The implementation worker has not run these account actions. The parent coordinates review and actual browser consent. `--help` is safe without reading or creating credential storage:

```powershell
$probe = './build/windows-debug/RackSynthOAuthProbe_artefacts/Debug/RackSynthOAuthProbe.exe'
& $probe --help
& $probe --sign-in
& $probe --status
& $probe --models
# Replace ACCOUNT_MODEL_SLUG with a slug from this account's --models result.
& $probe --request --model ACCOUNT_MODEL_SLUG
```

`--sign-in` starts an IPv4 listener on `127.0.0.1`, persists a stable installation UUID, and then opens the standard default browser. Complete consent in that browser. The callback displays only a fixed return-to-app message; inspect the sanitized console status for the result. Ctrl+C cancels. The authorization URL, callback query, token bodies, verified subject, and credential file contents are never printed.

`--request` submits one fixed warm-pad prompt with `store:false`, streaming, and a strict `text.format` JSON schema. It returns a candidate only after a complete, non-refused response whose local validation permits exactly `engine: analog|wavetable` and finite numeric `cutoff_hz` between 20 and 20000. The caller selects the inference model from the account catalog; this has no relationship to development-agent routing.

Inference parsing results retain the HTTP status and a bounded safe request ID. A rejected stream emits one fixed internal `error_code` (`sse_*` or `candidate_invalid`) identifying its existing validation branch. These codes never copy provider event names, property names, generated text, raw error bodies or headers. The diagnostics do not relax validation or establish the cause of a live failure without its sanitized result.

Exit 0 means a successful action or a saved-session status of `signed_out`; other status results exit 1, malformed CLI arguments exit 2. `--status` is local only: `connected` means a saved, unexpired connection with the necessary scopes, not newly verified network access. An expired saved access token yields `access_expired`. `--models` and `--request` can refresh on demand.

## Credentials, renewal, and limits

The human-run launcher stores only in `%LOCALAPPDATA%/AI Rack Synth/OAuthProbe`. It never reads Codex credentials or config. `session.bin` is encrypted with current-user DPAPI; its protected ACL permits the current user and SYSTEM. A flushed private temporary file is atomically moved into place. An exclusive `session.lock` file handle serializes registration, renewal, and saved-session operations across probe processes. Reparse-point paths are rejected. The lock wait is bounded to five seconds; another active process can yield `busy`.

New authorization uses fresh CNG random state, nonce and PKCE verifier. Callback attempts are one-time and expire in five minutes; the launcher waits up to three minutes for the browser. New registrations must return an issued client ID. Returning registrations cannot switch client or verified subject. Failed sign-in leaves the selected account credentials intact, including a returning consent that lacks direct inference permission. A first identity-only connection is reported as `inference_not_granted`, and cannot call inference.

ID tokens use real Windows CNG RS256 verification against the fixed trusted JWKS endpoint, then exact issuer, audience, expiry and nonce validation. Returning identity is bound to the verified subject. A refreshed ID token is verified when supplied; its nonce may be absent, but a present nonce must match the saved original. Expired access is renewed using the saved issued client, rotating refresh token and resource, with no scope parameter. Terminal `invalid_grant`/`invalid_client` clears unusable tokens while retaining identity and registration. Network, 5xx, rate-limit and scheduling errors preserve the record.

**Refresh timing ruling:** `earliest_refresh_at` remains an opaque preserved JSON value because its documented type and units are unresolved. Per the parent's explicit task ruling, this probe waits until the saved access expiry (`saved_at + expires_in`) before renewing. It does not implement proactive near-expiry scheduling or force early refresh. A provider scheduling rejection is reported as a sanitized known code. Real refresh acceptance is a live gate. If server rotation succeeds but local replacement fails, the old encrypted record remains intact; it may already be invalid server-side and can require sign-in again. Offline tests verify preservation, not server rollback.

WinHTTP accepts only the four pinned token/JWKS/models/responses URLs, validates TLS normally, disables redirects, cookies and automatic authentication, and never follows JWT-supplied key URLs. Async completion permits cancellation and deadline handling without closing a synchronous request from another thread. Callback context and buffers remain alive until the final `HANDLE_CLOSING` notification. See [Microsoft's WinHTTP concurrency contract](https://learn.microsoft.com/windows/win32/winhttp/concurrency-in-winhttp).

The request, connection and session have one heap-owned lifecycle. It retains the send body, read buffer and callback context independently of the caller. A single cleanup owner closes each handle at most once, in order, and stops at the first failed close; it never retries a close-called handle. Cancellation/deadline returns are bounded even if the close call itself blocks or final notification is delayed. Further native requests in that process are refused while cleanup remains unresolved. Successful cleanup plus the final callback releases ownership. A failed close or permanently missing callback intentionally quarantines one lifecycle for process lifetime; exit and restart the probe to recover. This trades one bounded retained allocation/handle hierarchy for avoiding use-after-free or reuse of an uncertain native handle. A late final callback releases a successfully closed hierarchy safely. See [WinHttpCloseHandle lifetime requirements](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle).

Successful cleanup retires its admission slot exactly once before reporting completion, independently of when the last lifecycle reference is destroyed. The next sequential request can start immediately. Old references, repeated completion signals and destruction cannot release a slot owned by a newer request. Uncertain cleanup still retains admission and remains quarantined.

Payload limits: callback headers/target 8 KiB, token/JWKS 256 KiB, catalog 1 MiB/1000 entries, SSE 4 MiB, candidate 16 KiB, JSON nesting 24. Requests use a 30-second deadline (60 seconds for inference), with five-second WinHTTP operation timeouts. Loopback connections allow two seconds per header and at most twelve connections per attempt. The parser requires a terminal `response.completed` with completed status. If its output array is empty, it validates the accumulated nonempty text deltas as the candidate, following the direct route's streaming example and the parent's sanitized actual stream evidence. Present terminal message text must match accumulated deltas exactly. Optional `response.output_text.done` text must match the final text; that event is not required. Candidate publication waits until the whole stream passes terminal, framing, refusal, failed/incomplete/error, malformed JSON, interruption, exact schema, duplicate-key and range checks.

See verification evidence and unrun live gates.
