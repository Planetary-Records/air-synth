"""Development-only, allowlisted AIR data archives. Native admission remains authoritative."""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import struct
import tempfile
import uuid
import zipfile
import zlib
import copy
import subprocess

MANIFEST_BYTES = DESIGN_BYTES = 65536
CATALOG_BYTES = 4 * 1024 * 1024
SKIN_BYTES = 1048576
ARCHIVE_BYTES = EXPANDED_BYTES = 384 * 1024 * 1024
FILE_COUNT = 256
PNG_BYTES, PNG_EDGE, PNG_PIXELS, DECODED_BYTES = 8 * 1024 * 1024, 8192, 16777216, 64 * 1024 * 1024
DRUM_BUNDLE_BYTES = 24 + 1048576 + 65536 + 256 * 1024 * 1024 + 32
LIMITS = {"design": DESIGN_BYTES, "skin": SKIN_BYTES, "png": PNG_BYTES, "notice": SKIN_BYTES,
          "preset": 828129, "audio": 20 + 480000 * 2 * 4 + 32, "drum-bundle": DRUM_BUNDLE_BYTES}
EXTENSIONS = {"design": {".json"}, "skin": {".json"}, "png": {".png"}, "notice": {".txt", ".md"},
              "preset": {".soundstate"}, "audio": {".airasset"}, "drum-bundle": {".airdrums"}}
MODULES = ("analog", "wavetable", "drive", "delay", "reverb", "compressor", "eq", "filter_fx", "chorus", "phaser",
           "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split",
           "dynamic_eq", "space_reverb", "era_reverb", "drum", "saturation")
ROOT = Path(__file__).resolve().parents[1]


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _bounded_bytes(path, maximum, label):
    with path.open("rb") as stream:
        _require(0 < os.fstat(stream.fileno()).st_size <= maximum, label + " byte limit")
        data = stream.read(maximum + 1)
    _require(0 < len(data) <= maximum, label + " byte limit")
    return data


def _closed(value, fields):
    return isinstance(value, dict) and set(value) == set(fields.split())


def _json_pairs(pairs):
    value = {}
    for key, item in pairs:
        _require(key not in value, "Duplicate JSON key")
        value[key] = item
    return value


def _read_json(data):
    def finite(token):
        number = float(token)
        _require(math.isfinite(number), "Non-finite JSON")
        return number

    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_json_pairs,
                          parse_float=finite,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Non-finite JSON")))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("Invalid UTF-8 JSON") from error


def _json_bytes(value):
    try:
        return (json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")
    except (UnicodeError, TypeError, ValueError) as error:
        raise ValueError("Invalid JSON value") from error


def _token(value, limit):
    return isinstance(value, str) and len(value) <= limit and re.fullmatch(r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?", value) is not None


def _version(value):
    if not isinstance(value, str) or len(value) > 32 or re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", value) is None:
        return None
    parts = tuple(map(int, value.split(".")))
    return parts if max(parts) <= 65535 else None


def _hash(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _path(value):
    if not isinstance(value, str) or not 0 < len(value) <= 160 or re.fullmatch(r"[A-Za-z0-9_./-]+", value) is None:
        return False
    for part in value.split("/"):
        lower = part.lower()
        base = lower.split(".")[0]
        if (not part or part.startswith(".") or part.endswith(".") or lower in {"package.json", "source.json", "rack-source.json"}
                or lower.startswith(("prompt", "private")) or "credential" in lower
                or base in {"con", "prn", "aux", "nul"} or re.fullmatch(r"(?:com|lpt)[0-9]", base)):
            return False
    return True


def _text(value, limit, empty=False):
    return (isinstance(value, str) and (empty or bool(value.strip())) and len(value.encode("utf-8")) <= limit
            and all(ord(c) >= 32 or c in "\n\t" for c in value))


def _clean_public_json(value, trail=()):
    if isinstance(value, dict):
        for key, item in value.items():
            words = re.findall(r"[a-z0-9]+", re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", key).casefold())
            compact = "".join(words)
            credentials = bool(set(words) & {"credential", "credentials", "password", "passwd", "secret", "secrets", "token", "tokens"})
            credentials |= "apikey" in compact or compact in {"accesstoken", "refreshtoken", "authtoken", "bearertoken", "clientsecret", "secretkey", "apisecret"}
            runtime_status = (trail == ("native_face",) and key == "status") or (trail == ("elements", "[]", "route") and key == "source")
            _require(not credentials and (runtime_status or compact not in {"source", "sourcepath", "sourcesha256", "command", "script", "provenance", "prompt", "status", "note", "notes", "conversion", "comparison"})
                     and not compact.startswith(("prompt", "private")), "Private provenance or credential metadata is not a release input")
            _clean_public_json(item, trail + (key,))
    elif isinstance(value, list):
        for item in value:
            _clean_public_json(item, trail + ("[]",))
    elif isinstance(value, str):
        value.encode("utf-8")
        path = value.replace("\\", "/").casefold()
        _require(re.search(r"(?:(?<![a-z0-9])[a-z]:/|(?<!:)//|/(?:users|home|root)/|(?:^|[/\s])(?:docs/(?:verification|design)|build)/)", path) is None,
                 "Private or absolute user path is not a release input")


def _validate_manifest(m):
    _require(_closed(m, "schema id version metadata design skin screenshot files capabilities") and type(m["schema"]) is int and m["schema"] == 1, "Invalid package schema/fields")
    _require(_token(m["id"], 64) and _version(m["version"]) is not None, "Invalid package ID/version")
    _require(isinstance(m["screenshot"], str) and _path(m["screenshot"]), "Native screenshot input is required")
    _require(_path(m["design"]) and isinstance(m["skin"], str) and (not m["skin"] or _path(m["skin"])), "Invalid design/skin path")
    meta = m["metadata"]
    _require(_closed(meta, "design revision canonical_sha256 name maker purpose description role tags"), "Invalid metadata fields")
    try:
        design_id = uuid.UUID(meta["design"])
    except (ValueError, TypeError, AttributeError) as error:
        raise ValueError("Invalid design UUID") from error
    _require(design_id.int != 0 and str(design_id) == meta["design"], "Invalid design UUID")
    _require(isinstance(meta["revision"], str) and re.fullmatch(r"[1-9][0-9]{0,19}", meta["revision"]) is not None
             and int(meta["revision"]) <= 2**64 - 1 and _hash(meta["canonical_sha256"]), "Invalid design revision/hash")
    for field, limit in (("name", 128), ("maker", 128), ("purpose", 512), ("description", 4096)):
        _require(_text(meta[field], limit, field == "description"), "Invalid metadata " + field)
    _require(meta["role"] in ("synth", "fx"), "Invalid rack role")
    tags = meta["tags"]
    _require(isinstance(tags, list) and len(tags) <= 8 and all(_token(t, 32) for t in tags) and len(set(tags)) == len(tags), "Invalid/duplicate tag IDs")
    vocabulary = _read_json((ROOT / "racks/tags.json").read_bytes())
    _require(_closed(vocabulary, "schema tags") and vocabulary["schema"] == 1 and isinstance(vocabulary["tags"], list)
             and len(vocabulary["tags"]) <= 128, "Invalid curated tag dictionary")
    known = set()
    for tag in vocabulary["tags"]:
        _require(_closed(tag, "id label") and _token(tag["id"], 32) and _text(tag["label"], 64) and tag["id"] not in known, "Invalid curated tag")
        known.add(tag["id"])
    _require(set(tags) <= known, "Uncurated tag ID")
    caps = m["capabilities"]
    _require(_closed(caps, "airMin modules nativeEditor") and _version(caps["airMin"]) is not None
             and _version(caps["airMin"]) <= (0, 1, 0) and caps["nativeEditor"] in ("", "space_reverb"), "Unsupported AIR/editor capability")
    _require(isinstance(caps["modules"], list) and 0 < len(caps["modules"]) <= 9, "Invalid module capabilities")
    seen = set()
    for module in caps["modules"]:
        _require(_closed(module, "module algorithmRevision") and type(module["module"]) is int and 0 <= module["module"] < len(MODULES)
                 and type(module["algorithmRevision"]) is int and 0 < module["algorithmRevision"] <= 255, "Invalid module capability")
        pair = (module["module"], module["algorithmRevision"])
        _require(pair not in seen, "Duplicate module capability")
        seen.add(pair)
    _require(caps["nativeEditor"] != "space_reverb" or any(m == 18 for m, _ in seen), "Editor module missing")
    files = m["files"]
    _require(isinstance(files, list) and 0 < len(files) <= FILE_COUNT - 1, "Package file count limit")
    names, total = {}, 0
    for f in files:
        _require(_closed(f, "path sha256 kind bytes") and _path(f["path"]) and _hash(f["sha256"])
                 and isinstance(f["kind"], str) and f["kind"] in LIMITS, "Invalid package file")
        _require(PurePosixPath(f["path"]).suffix.lower() in EXTENSIONS[f["kind"]], "Invalid data file kind/extension")
        _require(type(f["bytes"]) is int and 0 < f["bytes"] <= LIMITS[f["kind"]], "Package file byte limit")
        total += f["bytes"]
        _require(total <= EXPANDED_BYTES, "Expanded package byte limit")
        lower = f["path"].lower()
        _require(lower not in names, "Duplicate/case-colliding path")
        names[lower] = f
    for name in names:
        _require(all(str(parent) not in names for parent in PurePosixPath(name).parents if str(parent) != "."), "File/directory path collision")
    for field, kind in (("design", "design"), ("skin", "skin"), ("screenshot", "png")):
        if field == "skin" and not m[field]:
            continue
        _require(m[field].lower() in names and names[m[field].lower()]["path"] == m[field] and names[m[field].lower()]["kind"] == kind, "Missing listed " + field)
    _require(sum(f["kind"] == "design" for f in files) == 1 and sum(f["kind"] == "skin" for f in files) == bool(m["skin"]), "Invalid design/skin inventory")
    _require(names[m["design"].lower()]["sha256"] == meta["canonical_sha256"], "Recipe inventory hash mismatch")
    data = _json_bytes(m)
    _require(len(data) <= MANIFEST_BYTES and total + len(data) <= EXPANDED_BYTES, "Package manifest/expanded byte limit")
    _clean_public_json(m)
    return data


def _png_decoded(data):
    _require(33 <= len(data) <= PNG_BYTES and data[:16] == b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR", "Invalid PNG IHDR")
    width, height, depth, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", data[16:29])
    _require(width > 0 and height > 0 and max(width, height) <= PNG_EDGE and width * height <= PNG_PIXELS, "PNG geometry limit")
    allowed = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
    _require(depth in allowed.get(colour, set()) and compression == filtering == 0 and interlace <= 1
             and zlib.crc32(data[12:29]) == struct.unpack(">I", data[29:33])[0], "Invalid PNG header/CRC")
    return width * height * 4


def _source_files(source, manifest):
    mapping = {f["path"]: source / f["path"] for f in manifest["files"]}
    metadata = source / "rack-source.json"
    if metadata.exists() and not (source / "package.json").exists():
        seed = _read_json(metadata.read_bytes())
        _require(isinstance(seed, dict) and seed.get("schema") == 1 and isinstance(seed.get("sources"), list), "Invalid development source map")
        root = source.parent.parent
        _require(source == root / "racks" / manifest["id"], "Source map must be in its repository rack directory")
        mapping = {}
        for entry in seed["sources"]:
            _require(_closed(entry, "path source") and _path(entry["path"]) and _path(entry["source"]), "Invalid source map path")
            rel = PurePosixPath(entry["source"])
            _require(rel.is_relative_to(PurePosixPath("racks") / manifest["id"]) or rel.is_relative_to(PurePosixPath("resources/maker-skins") / manifest["id"]), "Source mapping leaves owned rack inputs")
            _require(entry["path"] not in mapping, "Duplicate source mapping")
            mapping[entry["path"]] = root / entry["source"]
        _require(set(mapping) == {f["path"] for f in manifest["files"]}, "Unlisted source mapping")
    return mapping


def build_package(source: Path, manifest: dict, output: Path) -> Path:
    """Build ID-version.airrack, refusing differing existing releases. No network or installation."""
    source, output = Path(source).absolute(), Path(output).absolute()
    manifest_bytes = _validate_manifest(manifest)
    mapping, payloads, decoded = _source_files(source, manifest), {}, 0
    for f in manifest["files"]:
        file = mapping[f["path"]]
        # Every path component remains plain: resolving a symlink must never expand the allowlist.
        _require(all(not p.is_symlink() and not p.is_junction() for p in (file, *file.parents)), "Linked source path is not allowed")
        _require(file.is_file() and file.stat().st_size == f["bytes"], "Missing or changed package input")
        data = file.read_bytes()
        _require(len(data) == f["bytes"] and hashlib.sha256(data).hexdigest() == f["sha256"], "Input SHA256/size mismatch")
        if f["kind"] == "png":
            decoded += _png_decoded(data)
            _require(decoded <= DECODED_BYTES, "Aggregate decoded PNG byte limit")
        elif f["kind"] in ("design", "skin"):
            document = _read_json(data)
            _require(isinstance(document, dict), "JSON data input must be an object")
            _clean_public_json(document)
            if f["kind"] == "design":
                meta = manifest["metadata"]
                _require(all(document.get(k) == meta[v] for k, v in (("id", "design"), ("revision", "revision"), ("role", "role"), ("name", "name"))), "Recipe metadata identity mismatch")
                units = document.get("units")
                _require(isinstance(units, list) and all(isinstance(u, dict) and u.get("module") in MODULES and type(u.get("algorithm_revision")) is int for u in units), "Invalid recipe module")
                actual = {(MODULES.index(u["module"]), u["algorithm_revision"]) for u in units}
                declared = {(u["module"], u["algorithmRevision"]) for u in manifest["capabilities"]["modules"]}
                _require(actual == declared, "Recipe capability mismatch")
        elif f["kind"] == "notice":
            _clean_public_json(data.decode("utf-8"))
        # Optional owned binary content is admitted by the existing native codecs at installation.
        payloads[f["path"]] = data
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for name, data in [("package.json", manifest_bytes), *sorted(payloads.items())]:
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system, info.external_attr = 3, 0o100644 << 16
            archive.writestr(info, data)
    data = buffer.getvalue()
    _require(len(data) <= ARCHIVE_BYTES, "Archive byte limit")
    output.mkdir(parents=True, exist_ok=True)
    target = output / (manifest["id"] + "-" + manifest["version"] + ".airrack")
    try:
        with tempfile.NamedTemporaryFile(prefix=".rack-package-", dir=output, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary, target)
        except FileExistsError:
            _require(target.is_file() and not target.is_symlink() and not target.is_junction()
                     and target.stat().st_size == len(data) and target.read_bytes() == data, "Existing ID/version artifact differs")
    finally:
        if "temporary" in locals():
            temporary.unlink(missing_ok=True)
    return target


def _plain_read(path: Path, maximum: int, label: str) -> bytes:
    _require(all(not p.is_symlink() and not p.is_junction() for p in (path, *path.parents)), "Linked source path is not allowed")
    return _bounded_bytes(path, maximum, label)


def prepare_manifest(source: Path) -> dict:
    """Refresh declared inputs in memory; writing the authoring manifest is an explicit CLI action."""
    source = source.absolute()
    manifest = _read_json(_plain_read(source / "package.json", MANIFEST_BYTES, "Manifest"))
    _validate_manifest(manifest)
    for entry in manifest["files"]:
        data = _plain_read(source / entry["path"], LIMITS[entry["kind"]], "Package file")
        entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    recipe = _read_json(_plain_read(source / manifest["design"], DESIGN_BYTES, "Recipe"))
    for field, key in (("design", "id"), ("revision", "revision"), ("name", "name"), ("role", "role")):
        manifest["metadata"][field] = recipe[key]
    manifest["metadata"]["canonical_sha256"] = next(f["sha256"] for f in manifest["files"] if f["path"] == manifest["design"])
    _validate_manifest(manifest)
    # Reuse every payload/PNG/recipe capability check before returning or rewriting anything.
    with tempfile.TemporaryDirectory() as temporary:
        build_package(source, manifest, Path(temporary))
    return manifest


def export_sources(source: Path, output: Path, previous: Path | None = None) -> dict:
    """Stage only reviewed authoring inputs, independent of the historical initial-release fixture."""
    _require(output.is_absolute(), "Source output must be absolute")
    _require(not output.exists(), "Source output must be a new empty directory")
    _require(all(not p.is_symlink() and not p.is_junction() for p in (output, *output.parents)), "Linked output path is not allowed")
    sources = sorted(source.absolute().glob("*/package.json"))
    _require(0 < len(sources) <= 512, "Source rack count limit")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage, index = Path(temporary) / "source", dict(schema=1, releases=[])
        stage.mkdir()
        docs = ROOT / "packaging/rack-library-source"
        if not docs.is_dir():
            docs = ROOT
        def write(path, data):
            target = stage / path
            _require(not str(path).startswith("releases/") or not target.exists() or target.read_bytes() == data,
                     "Immutable source release differs")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        if previous is not None:
            # A retained reviewed export, not an arbitrary checkout or ignored workspace.
            prior_files = {}
            for file in sorted(previous.rglob("*")):
                _require(not file.is_symlink() and not file.is_junction(), "Linked previous source path")
                path = file.relative_to(previous).as_posix()
                _require(public_library_path(path, "tree" if file.is_dir() else "blob"), "Unlisted previous source path")
                if file.is_file() and path != "SOURCE-CHECKSUMS.sha256":
                    data = _plain_read(file, ARCHIVE_BYTES, "Previous source file")
                    prior_files[path] = data
            inventory = "".join(hashlib.sha256(data).hexdigest() + "  " + path + "\n" for path, data in sorted(prior_files.items()))
            _require(_plain_read(previous / "SOURCE-CHECKSUMS.sha256", CATALOG_BYTES, "Previous inventory") == inventory.encode("utf-8"),
                     "Previous source inventory mismatch")
            for path, data in prior_files.items():
                if not path.startswith("racks/"):
                    write(path, data)
            index = _read_json(prior_files["SOURCE-INDEX.json"])
            _require(_closed(index, "schema releases") and index["schema"] == 1 and isinstance(index["releases"], list)
                     and len(index["releases"]) <= 32768, "Invalid previous source index")
            identities = set()
            for entry in index["releases"]:
                _require(_closed(entry, "id version sha256 path") and _token(entry["id"],64) and _version(entry["version"])
                         and _hash(entry["sha256"]) and entry["path"] == "releases/"+entry["id"]+"/"+entry["version"]+"/"+entry["sha256"],
                         "Invalid previous source identity")
                key = (entry["id"], entry["version"])
                _require(key not in identities and entry["path"] + "/package.json" in prior_files, "Duplicate or missing previous source release")
                identities.add(key)
        for name in ("README.md", "CONTRIBUTING.md", ".gitignore", ".gitattributes"):
            data = _plain_read(docs / name, SKIN_BYTES, "Contributor document")
            _clean_public_json(data.decode("utf-8"))
            write(name, data)
        write("LICENSE", _plain_read(ROOT / "LICENSE", SKIN_BYTES, "License"))
        write("tools/rack_packages.py", _plain_read(Path(__file__), SKIN_BYTES, "Packaging tool"))
        write("tests/test_rack_authoring.py", _plain_read(ROOT / "tests/test_rack_authoring.py", SKIN_BYTES, "Contributor tests"))
        write("racks/tags.json", _plain_read(ROOT / "racks/tags.json", SKIN_BYTES, "Tag vocabulary"))
        seen = set()
        for file in sources:
            rack = file.parent
            manifest = _read_json(_plain_read(file, MANIFEST_BYTES, "Manifest"))
            _validate_manifest(manifest)
            _require(rack.name == manifest["id"] and rack.name not in seen, "Source folder must match its unique rack ID")
            seen.add(rack.name)
            with tempfile.TemporaryDirectory() as archives:
                archive = build_package(rack, manifest, Path(archives))
                digest = hashlib.sha256(archive.read_bytes()).hexdigest()
                # The archive is the validated snapshot. An editor may already have changed the authoring files.
                with zipfile.ZipFile(archive) as package:
                    payloads = {entry["path"]: package.read(entry["path"]) for entry in manifest["files"]}
                    payloads["package.json"] = package.read("package.json")
            path = "releases/" + rack.name + "/" + manifest["version"] + "/" + digest
            prior = next((row for row in index["releases"] if row["id"] == rack.name and row["version"] == manifest["version"]), None)
            _require(prior is None or prior["sha256"] == digest, "Published package ID/version differs; increase the version")
            if prior is None:
                index["releases"].append(dict(id=rack.name, version=manifest["version"], sha256=digest, path=path))
            readme = _plain_read(rack / "README.md", SKIN_BYTES, "Rack guide")
            _clean_public_json(readme.decode("utf-8"))
            payloads["README.md"] = readme
            for name, data in payloads.items():
                write("racks/" + rack.name + "/" + name, data)
                write(path + "/" + name, data)
        index["releases"].sort(key=lambda row: (row["id"], _version(row["version"])))
        write("SOURCE-INDEX.json", _json_bytes(index))
        inventory = "".join(hashlib.sha256(file.read_bytes()).hexdigest() + "  " + file.relative_to(stage).as_posix() + "\n"
                            for file in sorted(stage.rglob("*"), key=lambda p: p.relative_to(stage).as_posix())
                            if file.is_file() and file.name != "SOURCE-CHECKSUMS.sha256")
        write("SOURCE-CHECKSUMS.sha256", inventory.encode("utf-8"))
        os.rename(stage, output)
    return index


def public_library_path(path: str, kind: str) -> bool:
    """Public data/source paths only; never workflows, native binaries or development maps."""
    if kind not in ("blob", "tree"):
        return False
    roots = {"README.md", "CONTRIBUTING.md", ".gitignore", ".gitattributes", "LICENSE", "NOTICES.txt", "catalog.json", "tags.json",
             "SOURCE-INDEX.json", "SOURCE-CHECKSUMS.sha256", "tools/rack_packages.py", "tests/test_rack_authoring.py", "racks/tags.json"}
    if path in roots:
        return kind == "blob"
    if kind == "tree" and path in {"tools", "tests", "racks", "releases", "notices"}:
        return True
    parts = path.split("/")
    if parts[0] == "notices":
        return _path(path) and (kind == "tree" or PurePosixPath(path).suffix in {".txt", ".md"})
    if len(parts) < 2 or parts[0] not in {"racks", "releases"} or not _token(parts[1], 64):
        return False
    if len(parts) == 2:
        return kind == "tree"
    start = 2
    if parts[0] == "releases":
        if not _version(parts[2]):
            return False
        if len(parts) == 3:
            return kind == "tree"
        if not _hash(parts[3]):
            return False
        if len(parts) == 4:
            return kind == "tree"
        start = 4
    relative = "/".join(parts[start:])
    if relative in {"package.json", "README.md"}:
        return kind == "blob"
    return _path(relative) and (kind == "tree" or PurePosixPath(relative).suffix in {extension for values in EXTENSIONS.values() for extension in values})


def author_sources(root: Path, captures: Path):
    """One development authoring pass; frozen inputs and private source maps are never rewritten."""
    frozen = _read_json((root / "tests/fixtures/rack-library/v1/manifest.json").read_bytes())
    runtime_keys = set("format version design_id maker model units rack_pitch native_face reveal pages strip readout assets elements groups narrow native_engraving extension_records".split())
    private_keys = set("source sourcepath sourcesha256 command script provenance prompt status note notes conversion comparison geometryauthority toolingsha256 coverage validation nativecontract".split())

    def sanitize(value, trail=()):
        if isinstance(value, dict):
            return {k: sanitize(v, trail + (k,)) for k, v in value.items()
                    if (trail == ("native_face",) and k == "status") or (trail == ("elements", "[]", "route") and k == "source") or re.sub(r"[^a-z0-9]", "", k.casefold()) not in private_keys}
        return [sanitize(v, trail + ("[]",)) for v in value] if isinstance(value, list) else value

    for identity in frozen["packages"]:
        source = root / "racks" / identity["id"]
        seed = _read_json((source / "rack-source.json").read_bytes())
        manifest = copy.deepcopy(seed["manifest"])
        payloads = {}
        for entry in seed["sources"]:
            data = (root / entry["source"]).read_bytes()
            original = next(f for f in seed["manifest"]["files"] if f["path"] == entry["path"])
            _require(len(data) == original["bytes"] and hashlib.sha256(data).hexdigest() == original["sha256"], "Frozen source identity differs")
            path = entry["path"]
            if original["kind"] == "skin":
                raw = _read_json(data)
                skin = sanitize({k: v for k, v in raw.items() if k in runtime_keys})
                # Tile's existing two reveal actions are declarative native controls, not executable extensions.
                if identity["id"] == "tile-array":
                    for record in skin["extension_records"]:
                        if record["proposed_kind"] in ("reveal-action", "reveal-close-action"):
                            skin["elements"].append(dict(kind="action", **{k: record[k] for k in ("id", "view", "label", "geometry")}))
                data = _json_bytes(skin)
            payloads[path] = (data, original["kind"])
        shot = (captures / identity["id"] / "screenshot.png").read_bytes()
        width, height = struct.unpack(">II", shot[16:24])
        _require(width >= 1080 and height >= 197, "Release screenshot must be a native rack capture")
        manifest["screenshot"] = "screenshot.png"
        payloads["screenshot.png"] = (shot, "png")
        payloads["notices/LICENSE.txt"] = ((root / "LICENSE").read_bytes(), "notice")
        payloads["notices/NOTICE.txt"] = (b"Original AIR rack definitions and artwork: AGPL-3.0-only.\nNative screenshot rendered by AIR. Maker names are fictional.\nThe screenshot includes AIR's native font rendering; accompanying font notices are retained.\nNo executable code, plugin or font file is supplied by this data package.\n", "notice")
        for name in ("IBMPlexSansCondensed-OFL.txt", "LICENSE.txt", "PermanentMarker-LICENSE.txt"):
            payloads["notices/font-" + name] = ((root / "resources/studio-skin/fonts" / name).read_bytes(), "notice")
        manifest["files"] = [dict(path=p, sha256=hashlib.sha256(d).hexdigest(), kind=k, bytes=len(d)) for p, (d, k) in sorted(payloads.items())]
        _validate_manifest(manifest)
        for path, (data, _) in payloads.items():
            target = source / path
            target.parent.mkdir(parents=True, exist_ok=True)
            _require(not target.is_symlink() and not target.is_junction(), "Linked authoring target")
            if target.exists():
                _require(target.read_bytes() == data, "Authoring target differs; review a new version")
            else:
                target.write_bytes(data)
        target = source / "package.json"
        data = _json_bytes(manifest)
        _require(not target.exists() or target.read_bytes() == data, "Existing manifest differs")
        target.write_bytes(data)


def _immutable_file(file, data):
    _require(not file.is_symlink() and not file.is_junction(), "Linked release output")
    if file.exists():
        _require(file.read_bytes() == data, "Existing release artifact differs")
    else:
        file.write_bytes(data)


def _identity(manifest, frozen):
    expected = next((p for p in frozen["packages"] if p["id"] == manifest["id"]), None)
    _require(expected is not None and manifest["version"] == expected["version"]
             and all(manifest["metadata"][k] == expected[k] for k in ("design", "revision", "canonical_sha256")), "Frozen release identity differs")


def build_release(source: Path, output: Path):
    _require(output.is_absolute(), "Release output must be absolute")
    source = source.absolute()
    frozen = _read_json((ROOT / "tests/fixtures/rack-library/v1/manifest.json").read_bytes())
    manifests = [_read_json((source / p["id"] / "package.json").read_bytes()) for p in frozen["packages"]]
    output.mkdir(parents=True, exist_ok=True)
    catalog = dict(schema=1, tags=_read_json((source / "tags.json").read_bytes())["tags"], racks=[])
    for order, manifest in enumerate(manifests):
        _identity(manifest, frozen)
        archive = build_package(source / manifest["id"], manifest, output)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        screenshot = source / manifest["id"] / manifest["screenshot"]
        name = manifest["id"] + "-" + manifest["version"] + ".png"
        _immutable_file(output / name, screenshot.read_bytes())
        url = "https://github.com/Planetary-Records/air-rack-library/releases/download/" + manifest["id"] + "-" + manifest["version"] + "/"
        release = dict(ref=dict(id=manifest["id"], version=manifest["version"], sha256=digest), metadata=manifest["metadata"], capabilities=manifest["capabilities"],
                       archive=dict(assetId=order + 1, url=url + archive.name, bytes=archive.stat().st_size, sha256=digest),
                       screenshot=dict(url=url + name, bytes=screenshot.stat().st_size, sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest()))
        catalog["racks"].append(dict(id=manifest["id"], order=order, releases=[release]))
    _immutable_file(output / "catalog.json", _json_bytes(catalog))
    files = sorted(p for p in output.iterdir() if p.name != "checksums.sha256")
    _require(all(p.is_file() for p in files), "Unexpected release directory")
    _immutable_file(output / "checksums.sha256", "".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n" for p in files).encode())
    return catalog


def verify_release(directory: Path):
    _require(directory.is_absolute() and directory.is_dir(), "Release input must be an existing absolute release directory")
    catalog_bytes = _bounded_bytes(directory / "catalog.json", CATALOG_BYTES, "Catalog")
    catalog = _read_json(catalog_bytes)
    digests = {"catalog.json": hashlib.sha256(catalog_bytes).hexdigest()}
    _require(_closed(catalog, "schema tags racks") and catalog["schema"] == 1 and len(catalog["racks"]) == 11, "Invalid release catalog")
    frozen = _read_json((ROOT / "tests/fixtures/rack-library/v1/manifest.json").read_bytes())
    _require(catalog["tags"] == _read_json((ROOT / "racks/tags.json").read_bytes())["tags"], "Release tag dictionary differs")
    expected = {"catalog.json", "checksums.sha256"}
    with tempfile.TemporaryDirectory() as temporary:
        for order, rack in enumerate(catalog["racks"]):
            _require(_closed(rack, "id order releases") and rack["order"] == order and len(rack["releases"]) == 1
                     and rack["id"] == frozen["packages"][order]["id"], "Invalid release order/identity")
            release = rack["releases"][0]
            _require(_closed(release, "ref metadata capabilities archive screenshot")
                     and _closed(release["ref"], "id version sha256") and release["ref"]["id"] == rack["id"]
                     and _closed(release["archive"], "assetId url bytes sha256") and release["archive"]["assetId"] == order + 1
                     and _closed(release["screenshot"], "url bytes sha256"), "Invalid candidate release bindings")
            name = rack["id"] + "-" + release["ref"]["version"] + ".airrack"
            file = directory / name
            data = _bounded_bytes(file, ARCHIVE_BYTES, "Archive")
            _require(len(data) <= ARCHIVE_BYTES and len(data) == release["archive"]["bytes"]
                     and hashlib.sha256(data).hexdigest() == release["ref"]["sha256"] == release["archive"]["sha256"], "Release archive identity differs")
            target = Path(temporary) / rack["id"]
            target.mkdir()
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                info = archive.infolist()
                _require(0 < len(info) <= FILE_COUNT and len({i.filename.casefold() for i in info}) == len(info), "Archive entry collision/count")
                entry = archive.getinfo("package.json")
                _require(0 < entry.file_size <= MANIFEST_BYTES, "Manifest byte limit")
                _require(entry.compress_type == zipfile.ZIP_STORED and entry.compress_size == entry.file_size
                         and entry.flag_bits & ~0x800 == 0, "Manifest ZIP encoding/compression differs")
                with archive.open(entry) as stream:
                    manifest_bytes = stream.read(MANIFEST_BYTES + 1)
                _require(0 < len(manifest_bytes) <= MANIFEST_BYTES, "Manifest byte limit")
                manifest = _read_json(manifest_bytes)
                _validate_manifest(manifest)
                _identity(manifest, frozen)
                _require(release["ref"]["version"] == manifest["version"], "Catalog version differs")
                _require(manifest["id"] == rack["id"] and release["metadata"] == manifest["metadata"] and release["capabilities"] == manifest["capabilities"], "Catalog/manifest differs")
                _require(set(archive.namelist()) == {"package.json", *(f["path"] for f in manifest["files"])}, "Archive allowlist differs")
                for item in manifest["files"]:
                    entry = archive.getinfo(item["path"])
                    _require(entry.file_size == item["bytes"] and entry.compress_type == zipfile.ZIP_STORED, "Archive size/compression differs")
                    payload = archive.read(entry)
                    path = target / item["path"]
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(payload)
                (target / "package.json").write_bytes(_json_bytes(manifest))
            rebuilt = build_package(target, manifest, Path(temporary) / "rebuilt")
            _require(rebuilt.read_bytes() == data, "Archive is not deterministic canonical output")
            image_name = rack["id"] + "-" + manifest["version"] + ".png"
            url = "https://github.com/Planetary-Records/air-rack-library/releases/download/" + rack["id"] + "-" + manifest["version"] + "/"
            _require(release["archive"]["url"] == url + name and release["screenshot"]["url"] == url + image_name, "Invalid planned candidate URL")
            shot = _bounded_bytes(directory / image_name, PNG_BYTES, "Screenshot")
            _require(shot == (target / manifest["screenshot"]).read_bytes() and len(shot) == release["screenshot"]["bytes"]
                     and hashlib.sha256(shot).hexdigest() == release["screenshot"]["sha256"], "Release screenshot differs")
            _require(struct.unpack(">II", shot[16:24])[0] >= 1080, "Release screenshot is not native size")
            expected.update((name, image_name))
            digests[name] = hashlib.sha256(data).hexdigest()
            digests[image_name] = hashlib.sha256(shot).hexdigest()
    _require({p.name for p in directory.iterdir()} == expected and all(not p.is_symlink() and not p.is_junction() for p in directory.iterdir()), "Unexpected release files/links")
    checksums = "".join(digests[n] + "  " + n + "\n" for n in sorted(expected - {"checksums.sha256"})).encode()
    _require(_bounded_bytes(directory / "checksums.sha256", len(checksums), "Checksum inventory") == checksums, "Release checksum inventory differs")
    return catalog


class PublicationError(RuntimeError):
    """The verified release could not be safely published."""


DISTRIBUTION_REPOSITORY = "Planetary-Records/air-rack-library"


def distribution_repository(requested: str) -> str:
    """Freeze the distribution owner to the configured private source remote and native endpoint."""
    remote = subprocess.run(["git", "remote", "get-url", "origin"], cwd=ROOT,
                            capture_output=True, text=True, check=True).stdout.strip()
    match = re.fullmatch(r"(?:https://github\.com/|git@github\.com:)([A-Za-z0-9-]+)/[A-Za-z0-9_.-]+?(?:\.git)?", remote)
    if (not match or (match[1] + "/air-rack-library").casefold() != DISTRIBUTION_REPOSITORY.casefold()
            or requested.casefold() != DISTRIBUTION_REPOSITORY.casefold()):
        raise PublicationError("Distribution must match the frozen native endpoint and configured source owner")
    return DISTRIBUTION_REPOSITORY


def _prepare_publication(output: Path, staging: Path):
    _require(output.is_absolute() and all(not p.is_symlink() and not p.is_junction()
                                        for p in (output, *output.parents)), "Publication input must be an absolute plain verified directory")
    catalog = verify_release(output)
    # Freeze a private copy, then verify it again: uploads never race the operator's input directory.
    for file in output.iterdir():
        (staging / file.name).write_bytes(_bounded_bytes(file, ARCHIVE_BYTES, "Publication input"))
    catalog = verify_release(staging)
    assets, notices = [], []
    for rack in catalog["racks"]:
        release = rack["releases"][0]
        stem = rack["id"] + "-" + release["ref"]["version"]
        assets.extend((staging / (stem + ".airrack"), staging / (stem + ".png")))
        with zipfile.ZipFile(assets[-2]) as archive:
            manifest = _read_json(archive.read("package.json"))
            for item in manifest["files"]:
                if item["kind"] == "notice":
                    notices.append((stem + "/" + item["path"]).encode() + b"\n" + archive.read(item["path"]) + b"\n")
    notice_bytes = b"AIR rack library - original package notices\n" + b"\n".join(notices)
    _require(0 < len(notice_bytes) <= CATALOG_BYTES, "Publication notices byte limit")
    notice = staging / ("notices-" + hashlib.sha256(notice_bytes).hexdigest() + ".txt")
    notice.write_bytes(notice_bytes)
    assets.append(notice)
    # The mutable catalog selection is deliberately excluded from immutable asset checksums.
    checksums = "".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n"
                        for p in sorted(assets)).encode()
    checksum = staging / ("checksums-" + hashlib.sha256(checksums).hexdigest() + ".sha256")
    checksum.write_bytes(checksums)
    assets.append(checksum)
    return catalog, assets


def publication_plan(output: Path, repository: str):
    repository = distribution_repository(repository)
    with tempfile.TemporaryDirectory() as temporary:
        _, assets = _prepare_publication(output, Path(temporary))
        return dict(status="DRY_RUN_NOT_PUBLISHED", repository=repository,
                    catalog="https://raw.githubusercontent.com/" + repository + "/main/catalog.json",
                    assets=[dict(name=p.name, bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in assets],
                    atomic_last=["catalog.json", "tags.json"],
                    bindings="Archive IDs and archive/screenshot URLs must come from verified GitHub responses; candidate bindings are never published")


def _asset_response(asset, name, sha256):
    prefix = "https://github.com/" + DISTRIBUTION_REPOSITORY + "/releases/download/"
    if (not isinstance(asset, dict) or type(asset.get("id")) is not int or asset["id"] <= 0
            or asset.get("sha256") != sha256 or not isinstance(asset.get("url"), str)
            or not asset["url"].startswith(prefix)
            or re.fullmatch(r"[A-Za-z0-9_.-]+/" + re.escape(name), asset["url"][len(prefix):]) is None):
        raise PublicationError("Unverified or conflicting GitHub asset response: " + name)
    return asset


class GitHubReleaseClient:
    """Small development-only gh adapter. File arguments and JSON bodies never enter a shell."""

    def __init__(self, repository: str):
        self.repository = distribution_repository(repository)
        self.endpoint = "repos/" + self.repository
        self.assets = None
        self.releases = {}
        self.verified_assets = {}
        self.catalog_commit = None

    def _api(self, endpoint, method="GET", body=None, file=None, binary=False):
        with tempfile.TemporaryDirectory() as temporary:
            args = ["gh", "api", "--hostname", "github.com", "--method", method, endpoint]
            if body is not None:
                file = Path(temporary) / "request.json"
                file.write_bytes(_json_bytes(body))
            if file is not None:
                args.extend(["--input", str(file)])
            if binary:
                args.extend(["--header", "Accept: application/octet-stream"])
                target = Path(temporary) / "asset"
                with target.open("wb") as stream:
                    result = subprocess.run(args, stdout=stream, stderr=subprocess.PIPE, check=False)
                if result.returncode:
                    raise PublicationError("GitHub asset download failed; catalog unchanged")
                return hashlib.sha256(_bounded_bytes(target, ARCHIVE_BYTES, "Remote asset")).hexdigest()
            if file is not None and body is None:
                args.extend(["--header", "Content-Type: application/octet-stream"])
            result = subprocess.run(args, capture_output=True, check=False)
            if result.returncode:
                raise PublicationError("GitHub API request failed; inspect operator credentials/access privately")
            return _read_json(result.stdout)

    def _head(self):
        return self._api(self.endpoint + "/git/ref/heads/main")["object"]["sha"]

    def _check_repository(self):
        repo = self._api(self.endpoint)
        if repo.get("private") is not False or repo.get("full_name", "").casefold() != self.repository.casefold() or repo.get("default_branch") != "main":
            raise PublicationError("Expected the reviewed public data repository with initialized main")
        head = self._head()
        commit = self._api(self.endpoint + "/git/commits/" + head)
        tree = self._api(self.endpoint + "/git/trees/" + commit["tree"]["sha"] + "?recursive=1")
        if tree.get("truncated") is not False or any(
                not public_library_path(item["path"], item["type"])
                for item in tree["tree"]):
            raise PublicationError("Public repository contains files outside data/source/notices allowlist")
        return head, commit["tree"]["sha"], tree["tree"]

    def _load_assets(self):
        if self.assets is not None:
            return
        self._check_repository()
        inventory, release_ids = {}, {}
        for page in range(1, 17):
            releases = self._api(self.endpoint + "/releases?per_page=100&page=" + str(page))
            for release in releases:
                if release.get("draft") is not False:
                    raise PublicationError("Draft releases cannot supply public catalog assets")
                release_ids[release["tag_name"]] = release["id"]
                for asset_page in range(1, 5):
                    assets = self._api(self.endpoint + "/releases/" + str(release["id"]) + "/assets?per_page=100&page=" + str(asset_page))
                    for asset in assets:
                        if asset["name"] in inventory:
                            raise PublicationError("Duplicate existing release asset name")
                        inventory[asset["name"]] = asset
                    if len(assets) < 100:
                        break
                else:
                    raise PublicationError("Incomplete release asset inventory")
            if len(releases) < 100:
                self.assets, self.releases = inventory, release_ids
                return
        raise PublicationError("Incomplete release inventory")

    def _verified(self, asset):
        name = asset["name"]
        digest = self._api(self.endpoint + "/releases/assets/" + str(asset["id"]), binary=True)
        result = dict(id=asset["id"], url=asset["browser_download_url"], sha256=digest)
        self.verified_assets[name] = result
        return result

    def existing_asset(self, name: str) -> dict | None:
        self._load_assets()
        return self._verified(self.assets[name]) if name in self.assets else None

    def upload_asset(self, path: Path, sha256: str) -> dict:
        self._load_assets()
        if hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
            raise PublicationError("Staged upload bytes changed")
        tag = path.stem
        if tag not in self.releases:
            release = self._api(self.endpoint + "/releases", "POST",
                                dict(tag_name=tag, target_commitish="main", name=tag, body="Curated AIR rack data and original notices.", draft=False, prerelease=False))
            self.releases[tag] = release["id"]
        endpoint = "https://uploads.github.com/" + self.endpoint + "/releases/" + str(self.releases[tag]) + "/assets?name=" + path.name
        asset = self._api(endpoint, "POST", file=path)
        if asset.get("name") != path.name:
            raise PublicationError("GitHub returned a different asset name")
        self.assets[path.name] = asset
        return self._verified(asset)

    def publish_catalog(self, path: Path) -> None:
        parent, base_tree, entries = self._check_repository()
        data = _bounded_bytes(path, CATALOG_BYTES, "Public catalog")
        catalog = _read_json(data)
        files = {"catalog.json": data, "tags.json": _json_bytes(dict(schema=1, tags=catalog["tags"]))}
        blobs = {entry["path"]: entry.get("sha") for entry in entries}
        unchanged = all(blobs.get(name) == hashlib.sha1(b"blob " + str(len(value)).encode() + b"\0" + value).hexdigest()
                        for name, value in files.items())
        if unchanged:
            self.catalog_commit = dict(commit=parent, sha256=hashlib.sha256(data).hexdigest(), unchanged=True)
            return
        tree = self._api(self.endpoint + "/git/trees", "POST", dict(base_tree=base_tree, tree=[
            dict(path=name, mode="100644", type="blob", content=value.decode("utf-8")) for name, value in files.items()]))
        # A fast-forward-only ref update selects both files together, or leaves the prior catalog active.
        commit = self._api(self.endpoint + "/git/commits", "POST", dict(message="Select verified AIR rack catalog", tree=tree["sha"], parents=[parent]))
        result = self._api(self.endpoint + "/git/refs/heads/main", "PATCH", dict(sha=commit["sha"], force=False))
        if result["object"]["sha"] != commit["sha"]:
            raise PublicationError("Catalog selection response differs; reconcile public ref before retry")
        self.catalog_commit = dict(commit=commit["sha"], sha256=hashlib.sha256(data).hexdigest())


def publish_release(output: Path, github: GitHubReleaseClient) -> None:
    """Verify, preflight all immutable IDs, upload/verify assets, then select the catalog atomically."""
    try:
        with tempfile.TemporaryDirectory() as temporary:
            catalog, assets = _prepare_publication(output, Path(temporary))
            existing, hashes = {}, {}
            for path in assets:
                hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
                asset = github.existing_asset(path.name)
                if asset is not None:
                    existing[path.name] = _asset_response(asset, path.name, hashes[path.name])
            for path in assets:
                if path.name not in existing:
                    existing[path.name] = _asset_response(github.upload_asset(path, hashes[path.name]), path.name, hashes[path.name])
            archive_ids = set()
            for rack in catalog["racks"]:
                release = rack["releases"][0]
                stem = rack["id"] + "-" + release["ref"]["version"]
                archive, screenshot = existing[stem + ".airrack"], existing[stem + ".png"]
                if archive["id"] in archive_ids:
                    raise PublicationError("Duplicate uploaded archive asset ID")
                archive_ids.add(archive["id"])
                release["archive"].update(assetId=archive["id"], url=archive["url"])
                release["screenshot"]["url"] = screenshot["url"]
            public_catalog = Path(temporary) / "catalog.json"
            public_catalog.write_bytes(_json_bytes(catalog))
            github.publish_catalog(public_catalog)
    except PublicationError:
        raise
    except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        raise PublicationError("Publication refused: " + str(error)) from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("--source", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--input", type=Path, required=True)
    publish = commands.add_parser("publish")
    publish.add_argument("--input", type=Path, required=True)
    publish.add_argument("--repository", required=True)
    publish.add_argument("--dry-run", action="store_true", help="Verify and print the exact public allowlist without credentials or network")
    prepare = commands.add_parser("prepare", help="Validate edited inputs and explicitly refresh package.json hashes")
    prepare.add_argument("--source", type=Path, required=True)
    single = commands.add_parser("build-rack", help="Validate and package one editable rack folder")
    single.add_argument("--source", type=Path, required=True)
    single.add_argument("--output", type=Path, required=True)
    export = commands.add_parser("export-source", help="Prepare an allowlisted public source tree; never uploads")
    export.add_argument("--source", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--previous", type=Path, help="Retained reviewed source export; preserve all immutable prior releases")
    preview = commands.add_parser("preview", help="Build a rack and open its actual native editor in a fresh private workspace")
    preview.add_argument("--source", type=Path, required=True)
    preview.add_argument("--workspace", type=Path, required=True)
    preview.add_argument("--executable", type=Path, required=True)
    preview.add_argument("--check", action="store_true", help="Capture the editor and check native render/recall, then exit")
    args = parser.parse_args()
    try:
        if args.command == "preview":
            _require(args.workspace.is_absolute() and not args.workspace.exists(), "Preview workspace must be a new absolute directory")
            _require(all(not p.is_symlink() and not p.is_junction() for p in (args.workspace, *args.workspace.parents)), "Linked preview workspace is not allowed")
            _require(args.executable.is_absolute() and args.executable.is_file(), "Preview executable must be an existing absolute path")
            # Keep package staging separate: the native process owns creating its new workspace.
            with tempfile.TemporaryDirectory() as temporary:
                archive = build_package(args.source, prepare_manifest(args.source), Path(temporary))
                command = [str(args.executable), "--archive", str(archive), "--workspace", str(args.workspace)]
                if args.check:
                    command.append("--check")
                result = subprocess.run(command, check=False)
                _require(result.returncode == 0, "Native preview failed (exit " + str(result.returncode) + "); retain the workspace for diagnosis")
            return
        if args.command == "prepare":
            manifest = prepare_manifest(args.source)
            # Replace only after validation; retain the previous file if validation failed.
            target = args.source / "package.json"
            with tempfile.NamedTemporaryFile(dir=args.source, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(_validate_manifest(manifest))
            try:
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
            print("Prepared " + manifest["id"] + " " + manifest["version"])
            return
        if args.command == "build-rack":
            _require(args.output.is_absolute(), "Package output must be absolute")
            archive = build_package(args.source, prepare_manifest(args.source), args.output)
            print(str(archive))
            print("SHA256 " + hashlib.sha256(archive.read_bytes()).hexdigest())
            return
        if args.command == "export-source":
            index = export_sources(args.source, args.output, args.previous)
            print(str(len(index["releases"])) + " source folders exported; NOT PUBLISHED")
            return
        if args.command == "publish":
            plan = publication_plan(args.input, args.repository)
            if args.dry_run:
                print(json.dumps(plan, indent=2), flush=True)
            else:
                github = GitHubReleaseClient(args.repository)
                publish_release(args.input, github)
                print(json.dumps(dict(status="PUBLISHED", repository=github.repository,
                                      catalog=github.catalog_commit, assets=github.verified_assets), indent=2))
            return
        catalog = build_release(args.source, args.output) if args.command == "build" else verify_release(args.input)
        print(str(len(catalog["racks"])) + " verified immutable rack releases (private candidate catalog)")
    except (ValueError, OSError, KeyError, PublicationError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
