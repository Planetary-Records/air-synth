# Direct Wide output research snapshot — unadopted

This is a portable source snapshot of the isolated round12 saturation experiment, not a product implementation or a newly qualified build. It changes no root CMake target, runtime, registry, saved state, rack UI, compiler option or acceptance gate. No native program or test was run while preparing this directory. All workers inherited the parent model without overrides.

## Source identity

`baseline/` contains the three differing files from the frozen paired-Horner ECC predecessor. Its Candidate SHA256 is `ecc591e7e84db9303da72efd5b658ad3344093a7e236b432eb28f55cc2a24353`. This is different from the tracked parent `probes/saturation/SaturationCandidate.cpp` (`85474f21620e4a3dd1ca4269f42bd4761a4fcf6700462c68f61949abc9d3113c`, the earlier packed core). Neither snapshot replaces that parent source.

`proposed/` contains the actual three-file direct-output proposal. `shared/` stores the five byte-identical companions once. The baseline and proposed private Wide APIs deliberately differ: the former returns four frames of three Quads; the latter writes three bands of four wet stereo frames and four frames of three dry stereo bands. The existing Quad components, equations and chronological histories are retained. These files are copied byte for byte; no newline normalization or reconstructed patch is needed.

`OriginalWrapper.cpp`, `ProposedWrapper.cpp` and `tests/` are the actual focused test sources. They compare output words and all72 state words, with canaries, asymmetric inputs, independent states, reset, seeded/changing coefficients and masked FP contexts. The witness prepares6,536 paired blocks,1,133 scenarios and2,119 clears. The tests have not been rerun from this relocated directory.

## Build boundary

There is no CMake registration or standalone build project here. A future reviewed test target must keep separate namespace owners: compile the old Wide implementation with `airack=wide_direct_original`, and the proposed Wide implementation with `airack=wide_direct_proposed`; their wrappers already select the matching private headers. Link the two wrappers and `tests/WideDirectWitness.cpp` with the repository's JUCE support. The test checks actual CPU/OS AVX admission and returns2 if unavailable; do not force admission or add an ISA/FP/FMA flag.

For any Candidate compilation, use that variant's directory before `shared/` on its include path: the variant supplies its matching Wide header and `shared/` supplies the unchanged public/helpers. The saved Feasibility source additionally uses the original repository `SaturationAnalysis.h/.cpp` and JUCE targets, which are not duplicated here. A future render executable must own one unrenamed Candidate, or independently renamed old/new owners, without linking conflicting `airack` types. Preserve the original scalar target/runtime/IPO options and private Wide `/GL-` boundary from the reviewed recipe; this source snapshot neither supplies nor certifies a new compiler recipe or linked image.

Run only the pure byte check, when wanted:

```text
python -B probes/saturation/experiments/direct-wide-20261019/verify.py
```

It reads this directory and its manifest only, checks exact file hashes and the three variant identities, and launches no compiler, test, native program, network call or subprocess. The verifier was prepared, not executed, as part of this source-only handoff. The generator checked each copied source against the original sealed manifest and byte-compared the copies. Historical repository input hashes are provenance, not a requirement that a later working tree remain at those bytes.

## Evidence and limits

The originating local seal is `8d35e1e45744ab829a98297968149f355e18281570a7c014711a84996ae33041`. Its absolute-path scripts, executables, DLLs, MAPs, disassembly, captures and large raw audit were intentionally omitted. The manifest lists relative origins and exact hashes so this directory can be checked without that ignored local tree. Earlier preparation records and failures remain local history; this snapshot makes no new test-first or qualification claim.

The tracked round12 report (private historical record, not distributed) and progress record (private historical record, not distributed) retain the actual evidence. Focused/original206/Diagnostic218/Curve/forced-nonWide and the two original linked-image codegen checks passed within their recorded scopes. The direct-output forward/reverse screens were order-dependent and did not warrant Full25: **SCREENED_NO_STANDALONE_PROMOTION**. No direct-output Full25 or official CPU gate ran. The latest predecessor ECC Full25 remains24/25 with CPU4/12FAIL; that predecessor is not adopted either.

All original gates remain: original206 absolute error<=1e-12, original curve/scaled checks,54 alias cases, callback allocation/free0/0 and all twelve eight-unit CPU cases at both-clock2s warm/10s measured, mean<=.35period and nearest-rank p99<=.5period. Product overlap gates and required hosted/CI/human acceptance remain separate. No selected factor, classifier composition, production DSP, host state, AI integration or actual rack-slot UI follows. The next authorized work is architectural/original-oracle feasibility discussion before another patch or native run.
