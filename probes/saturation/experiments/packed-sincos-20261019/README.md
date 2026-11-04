# Packed sine/cosine research snapshot

This is the exact isolated SSE2 proposal tested on2026-10-19, SHA256 `0548725a0b23d53851a7d755d43f926c00c16d86f5d162e32b330aa477cba83b`. The scoped `.gitattributes` preserves these exact CRLF bytes in Git. It is not registered in CMake or included by the product/root probe. No production adoption is implied.

Compare it with `../../SaturationMath.h`: the scalar sine/cosine Horner chains are packed into separate lanes with identical per-chain operations, constants, range reduction, guards and fallbacks. The separately rounded `r*z` is retained. Unsupported targets retain the literal scalar implementation. There is no new approximation, FMA, ISA flag, dependency or floating-point mode change.

The direct witness passed1,089,884 exact-bit/FP-environment pairs. Actual integrated Original206 passed the original absolute1e-12 limit; ECC218 was bit-identical across1,207,236 channel samples; Curve passed590,335 checks. Focused Release behavior and supplemental prepared C++ new/delete observations also pass. Each actual render executable was reviewed against its own sources, compiler recipe, MAP and emitted bytes; this file alone is not a new build or qualification.

The one forward/reverse CPU screen completed under substantial external Python/browser load and is contaminated/inconclusive. It establishes no intrinsic speed or regression result; no Full25 or adoption follows. Clean measurement is deferred. All original quality and CPU gates remain binding. See the experiment report (private historical record, not distributed) and plan (private historical record, not distributed) for the actual final decision, source/image identities, retained failures and limits. Large executables, raw data and absolute-path local build scaffolds remain ignored development evidence; this directory is a reviewable source snapshot, not a standalone reproduction package.

All workers inherited the parent chat model without overrides.
