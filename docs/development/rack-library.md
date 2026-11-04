# Rack library development

Racks are data packages describing existing modules, parameter bindings, original artwork and compatibility. They contain no executable code. Use canonical versioned packages; never replace bytes at an existing ID/version. Native admission remains authoritative.

The initial eleven identities are frozen. Build and verify into a fresh absolute directory:

```powershell
$rackOutput = Join-Path $PWD 'build/rack-library/reviewed-initial'
py -3.12 tools/rack_packages.py build --source "$PWD/racks" --output $rackOutput
py -3.12 tools/rack_packages.py verify --input $rackOutput
```

The aggregate verifier checks its quarantined candidate catalog, bounded payloads, canonical archives, native previews, notices and checksums. Its ordinal IDs are not actual GitHub asset IDs; never upload that catalog verbatim or weaken the fixture to make a real published catalog pass it. Publication constructs actual response-derived bindings and has a separate owner approval gate. No build/verify command installs data into a user's store.

A later revision needs a new semantic version, updated file hashes and canonical metadata, original/licensed inputs, native admission, installed control/recall checks and actual visual review. Keep prior versions and fixtures intact. The initial aggregate CLI is deliberately not a general new-cohort publisher; review that extension separately. Individual versioned archive authoring uses the existing build_package API.

The public data destination is Planetary-Records/air-rack-library. It contains catalog, tags, README, versioned data/previews and notices; no workflows, credentials, code or private source maps. A real anonymous HTTPS catalog/download/install check is required after public assets exist. Local fixtures and authenticated review downloads do not prove that interoperability.

For a new rack's musical purpose, art and bindings, see [rack contributions](racks.md). Manual playback must not gain an account or external-service requirement.
