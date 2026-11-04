# Accepted delay and earlier EQ draft

The seven delay files were captured by the actual accepted `4f43292` libraries (Sound20, Project19, 2063 parameters). Only a private fixture-writer function was added to the rack test executable; `capture-accepted-delay20.patch` reproduces that addition on the accepted checkout. No production source was changed. The registry is the complete accepted prefix, and the rich sound includes all 40 tape values, locks and continuous macro targets. The embedded files contain the actual Oxide Return example.

`draft-eq-flat-tilt-v19.airackproject` is an unchanged copy of the earlier EQ draft's actual listening input (Sound20, Project19, 2151 parameters). Its source and checksum are in `provenance.json`. The two Sound20 schemas are distinguished by exact count and key domain. Tests deliberately mutate copies to verify lock remapping, macro preservation and rejection of cross-schema keys; those mutations do not replace these original bytes.

The combined format appends EQ after the accepted delay prefix (2191 parameters, Sound21, Project20). Do not regenerate these fixtures with that writer. Capture commands, producer and file identities are recorded in `provenance.json`.
