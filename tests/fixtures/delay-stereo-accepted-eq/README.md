# Accepted EQ shapes format

The six files were captured by the actual accepted `49ae569` libraries (Sound21, Project20, 2191 parameters), after the EQ shapes and slopes merged. Only a private fixture-writer function was added to the rack test executable; `capture-accepted-eq21.patch` reproduces that addition on the accepted checkout. No production source was changed. The registry is the complete accepted prefix.

`rich-v21.soundstate` sets the 13 delay values of slot 2 (with its tape keys), all 128 EQ shape and slope extension values, alternate extension locks, a delay control lock, an effect lock and two continuous macro targets. The embedded files place the accepted Oxide Return example, whose recipe predates `delay.note`, on that sound.

The combined format appends the delay's three stereo keys after the accepted EQ prefix (2215 parameters, Sound22, Project21). Do not regenerate these fixtures with that writer. Capture commands, producer and file identities are recorded in `provenance.json`.
