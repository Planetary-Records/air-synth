import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile
import zlib
import subprocess
import sys
import shutil
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rack_packages", ROOT / "tools/rack_packages.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def png(width=1, height=1):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\0\0\0\0\0")) + chunk(b"IEND", b""))


class RackPackagesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / "source"
        self.source.mkdir()
        self.output = Path(self.temp.name) / "output"
        frozen = ROOT / "tests/fixtures/rack-library/v1/current/c83dab6a1c4e48c183c480d9212ca601-r1.json"
        self.recipe = frozen.read_bytes()
        self.design = json.loads(self.recipe)
        (self.source / "rack.json").write_bytes(self.recipe)
        (self.source / "screenshot.png").write_bytes(png())

    def fixture_manifest(self):
        files = []
        for name, kind in (("rack.json", "design"), ("screenshot.png", "png")):
            data = (self.source / name).read_bytes()
            files.append(dict(path=name, kind=kind, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        modules = sorted({(u["module"], u["algorithm_revision"]) for u in self.design["units"]})
        names = ["analog", "wavetable", "drive", "delay", "reverb", "compressor", "eq", "filter", "chorus", "phaser", "flanger", "stereo", "dimension", "frequencyShift", "convolution", "frequencySplit", "midSideSplit", "dynamicEq", "spaceReverb", "eraReverb", "drum", "saturation"]
        return dict(schema=1, id="ember-reed", version="1.0.0", design="rack.json", skin="", screenshot="screenshot.png", files=files,
                    metadata=dict(design=self.design["id"], revision=self.design["revision"], canonical_sha256=files[0]["sha256"], name="Ember Reed", maker="Fixture Maker", purpose="Boundary fixture", description="Fixture only", role="synth", tags=["analog", "warm"]),
                    capabilities=dict(airMin="0.1.0", modules=[dict(module=names.index(n), algorithmRevision=r) for n, r in modules], nativeEditor=""))

    def author_fixture(self, rack_id):
        root = Path(self.temp.name) / "authoring"
        frozen = json.loads((ROOT / "tests/fixtures/rack-library/v1/manifest.json").read_text())
        frozen["packages"] = [p for p in frozen["packages"] if p["id"] == rack_id]
        manifest = root / "tests/fixtures/rack-library/v1/manifest.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps(frozen))
        source = root / "racks" / rack_id
        source.mkdir(parents=True)
        shutil.copyfile(ROOT / "racks" / rack_id / "rack-source.json", source / "rack-source.json")
        seed = json.loads((source / "rack-source.json").read_text())
        for entry in seed["sources"]:
            target = root / entry["source"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / entry["source"], target)
        for name in ("LICENSE", "resources/studio-skin/fonts/IBMPlexSansCondensed-OFL.txt", "resources/studio-skin/fonts/LICENSE.txt", "resources/studio-skin/fonts/PermanentMarker-LICENSE.txt"):
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        captures = root / "captures" / rack_id
        captures.mkdir(parents=True)
        shutil.copyfile(ROOT / "racks" / rack_id / "screenshot.png", captures / "screenshot.png")
        skin_entry = next(e for e in seed["sources"] if e["path"] == seed["manifest"]["skin"])
        raw_file = root / skin_entry["source"]
        raw = json.loads(raw_file.read_text())
        raw["elements"][0]["status"] = "private authoring status"
        raw_file.write_text(json.dumps(raw))
        for entry in seed["manifest"]["files"]:
            if entry["path"] == skin_entry["path"]:
                entry["bytes"] = raw_file.stat().st_size
                entry["sha256"] = hashlib.sha256(raw_file.read_bytes()).hexdigest()
        (source / "rack-source.json").write_text(json.dumps(seed))
        packager.author_sources(root, captures.parent)
        skin = next(source.glob("skin/*json"))
        generated = json.loads(skin.read_text())
        original = json.loads((ROOT / next(e["source"] for e in seed["sources"] if e["path"] == seed["manifest"]["skin"])).read_text())
        self.assertNotIn("provenance", generated)
        self.assertNotIn("status", generated["elements"][0])

        return generated, original

    def test_authoring_preserves_native_face_runtime_status(self):
        generated, original = self.author_fixture("character-drive")
        self.assertEqual(generated["native_face"].get("status"), original["native_face"]["status"])

    def test_authoring_preserves_native_printed_route_source(self):
        generated, original = self.author_fixture("night-dial")
        sweep = next(e for e in generated["elements"] if e["id"] == "face.sweep")
        original_sweep = next(e for e in original["elements"] if e["id"] == "face.sweep")
        self.assertEqual(sweep["route"].get("source"), original_sweep["route"]["source"])

    def test_runtime_route_validation_is_narrow(self):
        accepted = True
        try:
            packager._clean_public_json({"elements": [{"route": {"source": "LFO 1"}}]})
        except ValueError:
            accepted = False
        self.assertTrue(accepted)
        for value in ({"source": "private"}, {"assets": {"source": "private"}}, {"elements": [{"source": "private"}]}, {"elements": [{"route": {"source": "C:/Users/private"}}]}):
            with self.assertRaises(ValueError):
                packager._clean_public_json(value)

    def test_runtime_status_validation_is_narrow(self):
        accepted = True
        try:
            packager._clean_public_json({"native_face": {"status": "FULL BANK"}})
        except ValueError:
            accepted = False
        self.assertTrue(accepted)
        for value in ({"status": "private"}, {"nested": {"status": "private"}}, {"native_face": {"nested": {"status": "private"}}}, {"native_face": {"status": "C:/Users/private"}}, {"native_face": {"api_key": "secret"}}):
            with self.assertRaises(ValueError):
                packager._clean_public_json(value)

    def test_deterministic_inventory(self):
        manifest = self.fixture_manifest()
        first = packager.build_package(self.source, manifest, self.output)
        data = first.read_bytes()
        second = packager.build_package(self.source, copy.deepcopy(manifest), Path(self.temp.name) / "other")
        self.assertEqual(data, second.read_bytes())
        self.assertEqual(first, packager.build_package(self.source, manifest, self.output))
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(archive.namelist(), ["package.json", "rack.json", "screenshot.png"])
            for entry in archive.infolist():
                self.assertEqual(entry.date_time, (1980, 1, 1, 0, 0, 0))
            self.assertEqual(archive.read("rack.json"), self.recipe)
            self.assertEqual(json.loads(archive.read("package.json")), manifest)

    def test_cli_requires_absolute_output_and_verifies_archive_bytes(self):
        tool=ROOT / "tools/rack_packages.py"
        result=subprocess.run([sys.executable,str(tool),"build","--source",str(ROOT / "racks"),"--output","relative"],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("absolute",result.stderr.lower())
        output=Path(self.temp.name) / "missing-release"
        result=subprocess.run([sys.executable,str(tool),"verify","--input",str(output)],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("release",result.stderr.lower())

    def test_real_release_cli_roundtrip_and_corruption_refusals(self):
        output=Path(self.temp.name) / "releases"
        packager.build_release(ROOT / "racks",output)
        catalog=packager.verify_release(output)
        self.assertEqual(len(catalog["racks"]),11)
        original=(output / "catalog.json").read_bytes()
        changed=copy.deepcopy(catalog)
        changed["racks"][0]["releases"][0]["ref"]["id"]="wrong-id"
        (output / "catalog.json").write_bytes(packager._json_bytes(changed))
        with self.assertRaisesRegex(ValueError,"bindings"):
            packager.verify_release(output)
        (output / "catalog.json").write_bytes(original)
        file=next(output.glob("*.airrack"))
        data=bytearray(file.read_bytes());data[-1]^=1;file.write_bytes(data)
        with self.assertRaisesRegex(ValueError,"identity"):
            packager.verify_release(output)

    def verification_fixture(self, manifest_bytes, compression=zipfile.ZIP_STORED, flags=0):
        output = Path(self.temp.name) / "verification"
        output.mkdir()
        frozen = json.loads((ROOT / "tests/fixtures/rack-library/v1/manifest.json").read_text())
        catalog = dict(schema=1, tags=json.loads((ROOT / "racks/tags.json").read_text())["tags"], racks=[])
        for order, identity in enumerate(frozen["packages"]):
            manifest = json.loads((ROOT / "racks" / identity["id"] / "package.json").read_text())
            release = dict(ref=dict(id=identity["id"], version=identity["version"], sha256="0" * 64),
                           metadata=manifest["metadata"], capabilities=manifest["capabilities"],
                           archive=dict(assetId=order + 1, url="", bytes=1, sha256="0" * 64),
                           screenshot=dict(url="", bytes=1, sha256="0" * 64))
            catalog["racks"].append(dict(id=identity["id"], order=order, releases=[release]))
        release = catalog["racks"][0]["releases"][0]
        archive = output / (release["ref"]["id"] + "-" + release["ref"]["version"] + ".airrack")
        with zipfile.ZipFile(archive, "w", compression=compression) as package:
            package.writestr("package.json", manifest_bytes)
        data = bytearray(archive.read_bytes())
        struct.pack_into("<H", data, 6, flags)
        struct.pack_into("<H", data, data.index(b"PK\x01\x02") + 8, flags)
        archive.write_bytes(data)
        release["archive"]["bytes"] = len(data)
        release["ref"]["sha256"] = release["archive"]["sha256"] = hashlib.sha256(data).hexdigest()
        (output / "catalog.json").write_bytes(packager._json_bytes(catalog))
        return output, archive

    def test_verify_rejects_raw_catalog_limit_before_json_decode(self):
        output = Path(self.temp.name) / "releases"
        packager.build_release(ROOT / "racks", output)
        catalog = output / "catalog.json"
        data = catalog.read_bytes()
        catalog.write_bytes(data + b" " * (4 * 1024 * 1024 + 1 - len(data)))
        files = sorted(p for p in output.iterdir() if p.name != "checksums.sha256")
        (output / "checksums.sha256").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n" for p in files))
        with mock.patch.object(packager, "_read_json", side_effect=AssertionError("JSON decoded before raw byte limit")):
            with self.assertRaisesRegex(ValueError, "Catalog byte limit"):
                packager.verify_release(output)

    def test_verify_rejects_raw_archive_limit_before_read(self):
        output, archive = self.verification_fixture(b"{}")
        original = Path.read_bytes
        def bounded_read(path):
            if path == archive:
                raise AssertionError("Archive read before raw byte limit")
            return original(path)
        # A reduced cap proves ordering without allocating a 384 MiB fixture.
        self.assertEqual(packager.ARCHIVE_BYTES, 384 * 1024 * 1024)
        with mock.patch.object(packager, "ARCHIVE_BYTES", archive.stat().st_size - 1), mock.patch.object(Path, "read_bytes", bounded_read):
            with self.assertRaisesRegex(ValueError, "Archive byte limit"):
                packager.verify_release(output)

    def test_verify_rejects_raw_manifest_limit_before_zip_open(self):
        output, _ = self.verification_fixture(b"{}" + b" " * (65536 - 1), zipfile.ZIP_DEFLATED)
        with mock.patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("Manifest opened before raw byte limit")):
            with self.assertRaisesRegex(ValueError, "Manifest byte limit"):
                packager.verify_release(output)

    def test_verify_rejects_manifest_compression_before_zip_open(self):
        output, _ = self.verification_fixture(b"{}", zipfile.ZIP_DEFLATED)
        with mock.patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("Manifest opened before compression check")):
            with self.assertRaisesRegex(ValueError, "Manifest ZIP encoding/compression"):
                packager.verify_release(output)

    def test_verify_rejects_manifest_flags_before_zip_open(self):
        output, _ = self.verification_fixture(b"{}", flags=1)
        with mock.patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("Manifest opened before encryption check")):
            with self.assertRaisesRegex(ValueError, "Manifest ZIP encoding/compression"):
                packager.verify_release(output)

    def test_legacy_map_and_compiled_exact_references_match_release_inputs(self):
        legacy=json.loads((ROOT / "racks/legacy-skins.json").read_text())
        compiled=(ROOT / "src/racks/RackExamples.cpp").read_text()
        for entry in legacy["entries"]:
            source=ROOT / "racks" / entry["id"]
            manifest=json.loads((source / "package.json").read_text())
            self.assertEqual(manifest["metadata"]["canonical_sha256"],entry["canonical_sha256"])
            self.assertIn('"'+entry["sha256"]+'"',compiled)
            self.assertIn('"'+entry["canonical_sha256"]+'"',compiled)

    def test_private_file_is_not_packaged(self):
        (self.source / "private-notes.txt").write_bytes(b"not a release input")
        (self.source / "SOURCE.json").write_text('{"private":"input"}')
        archive = packager.build_package(self.source, self.fixture_manifest(), self.output)
        with zipfile.ZipFile(archive) as package:
            self.assertNotIn("private-notes.txt", package.namelist())
            self.assertNotIn("SOURCE.json", package.namelist())

    def test_refuses_private_or_unsafe_listed_path(self):
        for name in ("SOURCE.json", "private-notes.txt", "prompt.txt", "../escape.png", "C:/user.png", "skin/CON.png", "skin/a.png:stream", "skin\\x.png"):
            with self.subTest(name=name):
                manifest = self.fixture_manifest()
                manifest["files"][1]["path"] = name
                manifest["screenshot"] = name
                with self.assertRaises(ValueError):
                    packager.build_package(self.source, manifest, self.output)

    def test_refuses_changed_immutable_artifact(self):
        manifest = self.fixture_manifest()
        archive = packager.build_package(self.source, manifest, self.output)
        kept = archive.read_bytes()
        manifest["metadata"]["description"] = "Changed bytes"
        with self.assertRaises(ValueError):
            packager.build_package(self.source, manifest, self.output)
        self.assertEqual(archive.read_bytes(), kept)

    def test_refuses_bad_hash_missing_or_screenshot(self):
        for change in ("hash", "missing", "screenshot", "executable", "oversized"):
            with self.subTest(change=change):
                manifest = self.fixture_manifest()
                if change == "hash": manifest["files"][0]["sha256"] = "0" * 64
                if change == "missing": manifest["files"][1]["path"] = "missing.png"
                if change == "screenshot": manifest["screenshot"] = ""
                if change == "executable": manifest["files"][1]["kind"] = "executable"
                if change == "oversized": manifest["files"][0]["bytes"] = 65537
                with self.assertRaises(ValueError): packager.build_package(self.source, manifest, self.output)

    def add_file(self, manifest, path, kind, data):
        file = self.source / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        manifest["files"].append(dict(path=path, kind=kind, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))

    def test_private_provenance_cannot_enter_skin(self):
        for document in ({"source": "private.png"}, {"source_sha256": "0" * 64}, {"note": "development evidence"},
                         {"asset": "C:/Users/private/file.png"}, {"asset": "docs/verification/private.png"}):
            manifest = self.fixture_manifest()
            self.add_file(manifest, "skin/elements.json", "skin", json.dumps(document).encode())
            manifest["skin"] = "skin/elements.json"
            with self.assertRaisesRegex(ValueError, "Private"):
                packager.build_package(self.source, manifest, self.output)

    def test_original_license_urls_are_allowed_and_private_drives_refuse(self):
        manifest = self.fixture_manifest()
        self.add_file(manifest,"notices/license.txt","notice",b"Original notice: https://www.gnu.org/licenses/\n")
        self.assertTrue(packager.build_package(self.source,manifest,self.output).is_file())
        with self.assertRaises(ValueError):
            packager._clean_public_json("Private input C:/Users/private/file.png")

    def test_i1_credentials_and_private_paths_refuse_before_publication(self):
        documents = [
            # The independent review's four exact probes.
            {"credentials": {"api_key": "fixture-secret"}},
            {"asset": r"\\server\Users\private\file.png"},
            {"asset": r"docs\verification\private.png"},
            {"asset": "/root/private/file.png"},
            {"metadata": {"API-Key": "fixture-secret"}},
            {"metadata": {"apiKey": "fixture-secret"}},
            {"metadata": {"PASSWORD": "fixture-secret"}},
            {"metadata": {"clientSecret": "fixture-secret"}},
            {"metadata": {"access_token": "fixture-secret"}},
            {"metadata": {"refresh-token": "fixture-secret"}},
            {"metadata": {"provider.credentials": {"value": "fixture-secret"}}},
            {"asset": r"DOCS\VERIFICATION\private.png"},
            {"asset": r"DoCs\DeSiGn\private.png"},
            {"asset": r"BuIlD\private.png"},
            {"asset": r"\\SERVER\SHARE\private.png"},
            {"asset": "//server/SHARE/private.png"},
            {"asset": "/ROOT/private/file.png"},
            {"asset": "/HOME/private/file.png"},
            {"asset": r"c:\USERS\private\file.png"},
        ]
        for index, document in enumerate(documents):
            with self.subTest(document=document):
                manifest = self.fixture_manifest()
                self.add_file(manifest, "skin/elements.json", "skin", json.dumps(document).encode())
                manifest["skin"] = "skin/elements.json"
                output = self.output / str(index)
                with self.assertRaisesRegex(ValueError, "Private"):
                    packager.build_package(self.source, manifest, output)
                self.assertFalse((output / "ember-reed-1.0.0.zip").exists())

    def test_clean_declarative_skin_is_allowed(self):
        manifest = self.fixture_manifest()
        skin = {"format": "ai-rack-maker-skin", "version": 1, "elements": [
            {"id": "cutoff", "key": "analog.filter.cutoff", "label": "Filter cutoff"},
            {"id": "source", "key": "modulation.1.source", "label": "Source"},
            {"id": "character", "key": "saturation.full.character", "label": "Character"},
        ]}
        self.add_file(manifest, "skin/elements.json", "skin", json.dumps(skin).encode())
        manifest["skin"] = "skin/elements.json"
        archive = packager.build_package(self.source, manifest, self.output)
        with zipfile.ZipFile(archive) as package:
            self.assertIn("skin/elements.json", package.namelist())
            self.assertEqual(json.loads(package.read("skin/elements.json")), skin)

    def test_curated_tag_case_duplicates_and_count(self):
        for tags in (["uncurated-tag"], ["Analog"], ["analog", "analog"], ["analog"] * 9, ["a" * 33]):
            manifest = self.fixture_manifest()
            manifest["metadata"]["tags"] = tags
            with self.assertRaises(ValueError):
                packager.build_package(self.source, manifest, self.output)

    def test_declared_and_actual_limits(self):
        for kind in packager.LIMITS:
            manifest = self.fixture_manifest()
            suffix = sorted(packager.EXTENSIONS[kind])[0]
            manifest["files"].append(dict(path="data/too-large" + suffix, kind=kind, bytes=packager.LIMITS[kind] + 1, sha256="a" * 64))
            with self.assertRaises(ValueError):
                packager.build_package(self.source, manifest, self.output)
        manifest = self.fixture_manifest()
        for i in range(254):
            manifest["files"].append(dict(path=f"notices/{i}.txt", kind="notice", bytes=1, sha256="a" * 64))
        with self.assertRaisesRegex(ValueError, "count"):
            packager.build_package(self.source, manifest, self.output)
        manifest = self.fixture_manifest()
        self.add_file(manifest, "skin/giant.png", "png", png(8193, 1))
        with self.assertRaisesRegex(ValueError, "geometry"):
            packager.build_package(self.source, manifest, self.output)
        manifest = self.fixture_manifest()
        for i in range(2):
            self.add_file(manifest, f"skin/large-{i}.png", "png", png(8192, 2048))
        with self.assertRaisesRegex(ValueError, "Aggregate"):
            packager.build_package(self.source, manifest, self.output)

    def test_source_mapping_is_owned_and_not_archived(self):
        source = Path(self.temp.name) / "repo/racks/ember-reed"
        source.mkdir(parents=True)
        (source / "rack.json").write_bytes(self.recipe)
        (source / "screenshot.png").write_bytes(png())
        seed = dict(schema=1, sources=[dict(path=p, source="racks/ember-reed/" + p) for p in ("rack.json", "screenshot.png")])
        metadata = source / "rack-source.json"
        metadata.write_text(json.dumps(seed))
        archive = packager.build_package(source, self.fixture_manifest(), self.output)
        with zipfile.ZipFile(archive) as package:
            self.assertNotIn("rack-source.json", package.namelist())
        for path in ("../outside.png", "C:/Users/private.png", "docs/verification/private.png", "racks/another-rack/file.png"):
            seed["sources"][1]["source"] = path
            metadata.write_text(json.dumps(seed))
            with self.assertRaises(ValueError):
                packager.build_package(source, self.fixture_manifest(), self.output)

    def test_strict_json_input(self):
        for data in (b'{"x":1,"x":2}', b'{"x":1e9999}', b'{"x":NaN}', b'{"x":"\xff"}'):
            with self.assertRaises(ValueError):
                packager._read_json(data)

    def test_frozen_seeds_and_curated_tags(self):
        frozen = json.loads((ROOT / "tests/fixtures/rack-library/v1/manifest.json").read_text())
        self.assertEqual(len(frozen["current"]), 28)
        self.assertEqual(len(frozen["packages"]), 11)
        tags = json.loads((ROOT / "racks/tags.json").read_text())
        vocabulary = {t["id"] for t in tags["tags"]}
        self.assertIn("drums", vocabulary)
        self.assertIn("saturator", vocabulary)
        for identity in frozen["packages"]:
            source = ROOT / "racks" / identity["id"]
            seed = json.loads((source / "rack-source.json").read_text())
            self.assertEqual(hashlib.sha256((source / "rack.json").read_bytes()).hexdigest(), identity["canonical_sha256"])
            self.assertLessEqual(len(seed["manifest"]["metadata"]["tags"]), 8)
            self.assertTrue(set(seed["manifest"]["metadata"]["tags"]) <= vocabulary)
            if "drums" in seed["manifest"]["metadata"]["tags"]:
                self.assertEqual(seed["manifest"]["metadata"]["role"], "synth")
            with self.assertRaisesRegex(ValueError, "screenshot"):
                packager.build_package(source, seed["manifest"], self.output)


class StubGitHub:
    repository = "Planetary-Records/air-rack-library"

    def __init__(self):
        self.assets = {}
        self.catalog = b"previous catalog"
        self.events = []
        self.failed_asset = None

    def existing_asset(self, name):
        self.events.append(("existing", name))
        return self.assets.get(name)

    def upload_asset(self, path, sha256):
        self.events.append(("upload", path.name))
        if path.name == self.failed_asset:
            raise packager.PublicationError("Injected upload interruption")
        asset = dict(id=9000 + len(self.assets), url="https://github.com/" + self.repository +
                     "/releases/download/verified-fixture/" + path.name, sha256=sha256)
        self.assets[path.name] = asset
        return asset

    def publish_catalog(self, path):
        self.events.append(("catalog", path.name))
        self.catalog = path.read_bytes()


class RackPublicationTests(unittest.TestCase):
    def test_publish_cli_stdout_is_one_json_receipt(self):
        github = mock.Mock(repository="Planetary-Records/air-rack-library",
                           catalog_commit="fixture-commit", verified_assets=[])
        import contextlib
        import io
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", ["rack_packages.py", "publish", "--input", str(self.verified_output),
                                            "--repository", github.repository]), \
                mock.patch.object(packager, "GitHubReleaseClient", return_value=github), \
                mock.patch.object(packager, "publish_release"), contextlib.redirect_stdout(stdout):
            packager.main()
        self.assertEqual(json.loads(stdout.getvalue())["status"], "PUBLISHED")

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.verified_output = Path(cls.temp.name) / "releases"
        packager.build_release(ROOT / "racks", cls.verified_output)

    def setUp(self):
        self.github = StubGitHub()

    def test_failed_upload_preserves_previous_catalog(self):
        previous = self.github.catalog
        self.github.failed_asset = "ember-reed-1.0.0.airrack"
        with self.assertRaises(packager.PublicationError):
            packager.publish_release(self.verified_output, self.github)
        self.assertEqual(self.github.catalog, previous)
        self.assertFalse(any(event[0] == "catalog" for event in self.github.events))

    def test_success_uses_verified_response_bindings_and_exact_allowlist(self):
        candidate = (self.verified_output / "catalog.json").read_bytes()
        packager.publish_release(self.verified_output, self.github)
        published = json.loads(self.github.catalog)
        self.assertEqual(self.github.events[-1], ("catalog", "catalog.json"))
        names = set(self.github.assets)
        self.assertEqual(len(names), 24)
        self.assertEqual(len([n for n in names if n.endswith(".airrack")]), 11)
        self.assertEqual(len([n for n in names if n.endswith(".png")]), 11)
        self.assertEqual(len([n for n in names if n.startswith("notices-")]), 1)
        self.assertEqual(len([n for n in names if n.startswith("checksums-")]), 1)
        self.assertNotIn("catalog.json", names)
        self.assertNotIn("checksums.sha256", names)
        self.assertEqual((self.verified_output / "catalog.json").read_bytes(), candidate)
        for rack in published["racks"]:
            release = rack["releases"][0]
            stem = rack["id"] + "-" + release["ref"]["version"]
            self.assertEqual(release["archive"]["assetId"], self.github.assets[stem + ".airrack"]["id"])
            self.assertGreaterEqual(release["archive"]["assetId"], 9000)
            self.assertEqual(release["archive"]["url"], self.github.assets[stem + ".airrack"]["url"])
            self.assertEqual(release["screenshot"]["url"], self.github.assets[stem + ".png"]["url"])
        self.assertNotEqual(self.github.catalog, candidate)

    def test_collision_preflight_aborts_without_upload_or_catalog_mutation(self):
        name = "ember-reed-1.0.0.airrack"
        self.github.assets[name] = dict(id=123, sha256="0" * 64,
            url="https://github.com/Planetary-Records/air-rack-library/releases/download/ember-reed-1.0.0/" + name)
        with self.assertRaises(packager.PublicationError):
            packager.publish_release(self.verified_output, self.github)
        self.assertEqual(self.github.catalog, b"previous catalog")
        self.assertTrue(all(event[0] == "existing" for event in self.github.events))

    def test_retry_reuses_identical_assets_and_retains_old_assets(self):
        self.github.assets["old-retained-0.9.0.airrack"] = dict(id=12, sha256="a" * 64, url="unused")
        self.github.failed_asset = "ember-reed-1.0.0.airrack"
        with self.assertRaises(packager.PublicationError):
            packager.publish_release(self.verified_output, self.github)
        uploaded_before = {name for kind, name in self.github.events if kind == "upload" and name != self.github.failed_asset}
        self.github.failed_asset = None
        self.github.events.clear()
        packager.publish_release(self.verified_output, self.github)
        self.assertTrue(uploaded_before.isdisjoint({name for kind, name in self.github.events if kind == "upload"}))
        previous = self.github.catalog
        self.github.events.clear()
        packager.publish_release(self.verified_output, self.github)
        self.assertFalse(any(kind == "upload" for kind, _ in self.github.events))
        self.assertEqual(self.github.catalog, previous)
        self.assertIn("old-retained-0.9.0.airrack", self.github.assets)

    def test_private_input_or_extra_file_refuses_before_github_calls(self):
        extra = self.verified_output / "SOURCE.json"
        try:
            extra.write_text('{"token":"fixture-not-a-real-token"}')
            with self.assertRaises(packager.PublicationError):
                packager.publish_release(self.verified_output, self.github)
        finally:
            extra.unlink()
        self.assertEqual(self.github.events, [])
        with self.assertRaises(packager.PublicationError):
            packager.publish_release(ROOT / "racks", self.github)
        self.assertEqual(self.github.events, [])

    def test_dry_run_never_calls_gh_and_reports_every_public_file(self):
        def local_git_only(args, **kwargs):
            self.assertEqual(args, ["git", "remote", "get-url", "origin"])
            return subprocess.CompletedProcess(args, 0, "https://github.com/Planetary-Records/air\n", "")
        with mock.patch.object(packager.subprocess, "run", side_effect=local_git_only):
            plan = packager.publication_plan(self.verified_output, self.github.repository)
        self.assertEqual(plan["status"], "DRY_RUN_NOT_PUBLISHED")
        self.assertEqual(len(plan["assets"]), 24)
        self.assertEqual(plan["atomic_last"], ["catalog.json", "tags.json"])
        for asset in plan["assets"]:
            self.assertRegex(asset["sha256"], "^[0-9a-f]{64}$")
            self.assertGreater(asset["bytes"], 0)

    def test_response_hash_url_and_id_must_be_verified(self):
        name = "ember-reed-1.0.0.airrack"
        correct = dict(id=9876, sha256="a" * 64,
            url="https://github.com/Planetary-Records/air-rack-library/releases/download/real-tag/" + name)
        for field, wrong in (("id", True), ("id", 0), ("sha256", "b" * 64),
                             ("url", "https://github.com/other/air-rack-library/releases/download/a/" + name),
                             ("url", correct["url"] + "?token=private")):
            with self.subTest(field=field, value=wrong):
                asset = dict(correct, **{field: wrong})
                with self.assertRaises(packager.PublicationError):
                    packager._asset_response(asset, name, "a" * 64)

    def test_destination_cannot_be_arbitrary_or_differ_from_native_endpoint(self):
        for remote, requested in (("https://github.com/other/air", self.github.repository),
                                  ("https://github.com/Planetary-Records/air", "other/air-rack-library"),
                                  ("https://token@github.com/Planetary-Records/air", self.github.repository)):
            with self.subTest(remote=remote), mock.patch.object(packager.subprocess, "run",
                    return_value=subprocess.CompletedProcess([], 0, remote, "")):
                with self.assertRaises(packager.PublicationError):
                    packager.distribution_repository(requested)
        native = (ROOT / "src/racks/RackLibraryClient.h").read_text()
        owner, repository = packager.DISTRIBUTION_REPOSITORY.split("/")
        self.assertIn('rackLibraryOwner="' + owner + '"', native)
        self.assertIn('rackLibraryRepository="' + repository + '"', native)


class GitHubAdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.calls = []
        self.files = {"README.md": b"AIR public rack data\n"}
        self.tree = "old-tree"
        self.head = "old-head"
        self.pending = None
        self.fail_ref = False
        self.asset_data = b"verified remote bytes"
        self.patcher = mock.patch.object(packager.subprocess, "run", side_effect=self.gh)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        self.client = packager.GitHubReleaseClient("Planetary-Records/air-rack-library")

    def gh(self, args, **kwargs):
        if args[0] == "git":
            return subprocess.CompletedProcess(args, 0, "https://github.com/Planetary-Records/air", "")
        self.assertEqual(args[:4], ["gh", "api", "--hostname", "github.com"])
        self.assertNotIn("shell", kwargs)
        method, endpoint = args[5:7]
        body = None
        if "--input" in args:
            body = Path(args[args.index("--input") + 1]).read_bytes()
        self.calls.append((method, endpoint, body))
        root = "repos/Planetary-Records/air-rack-library"
        if "stdout" in kwargs:
            kwargs["stdout"].write(self.asset_data)
            return subprocess.CompletedProcess(args, 0, None, b"")
        if endpoint == root:
            result = dict(private=False, full_name="Planetary-Records/air-rack-library", default_branch="main")
        elif endpoint == root + "/git/ref/heads/main":
            result = dict(object=dict(sha=self.head))
        elif endpoint == root + "/git/commits/" + self.head:
            result = dict(tree=dict(sha=self.tree))
        elif endpoint == root + "/git/trees/" + self.tree + "?recursive=1":
            result = dict(truncated=False, tree=[dict(path=name, type="blob", sha=hashlib.sha1(
                b"blob " + str(len(value)).encode() + b"\0" + value).hexdigest()) for name, value in self.files.items()])
        elif endpoint == root + "/git/trees" and method == "POST":
            data = json.loads(body)
            self.assertEqual(data["base_tree"], self.tree)
            self.pending = {entry["path"]: entry["content"].encode() for entry in data["tree"]}
            result = dict(sha="new-tree")
        elif endpoint == root + "/git/commits" and method == "POST":
            self.assertEqual(json.loads(body)["parents"], [self.head])
            result = dict(sha="new-head")
        elif endpoint == root + "/git/refs/heads/main" and method == "PATCH":
            self.assertIs(json.loads(body)["force"], False)
            if self.fail_ref:
                return subprocess.CompletedProcess(args, 1, b"", b"injected fast-forward failure")
            self.files.update(self.pending)
            self.head, self.tree = "new-head", "new-tree"
            result = dict(object=dict(sha=self.head))
        elif endpoint.startswith("https://uploads.github.com/" + root + "/releases/123/assets?name=") and method == "POST":
            self.assertEqual(body, self.asset_data)
            name = endpoint.split("?name=", 1)[1]
            result = dict(id=456, name=name, browser_download_url="https://github.com/Planetary-Records/air-rack-library/releases/download/real/" + name)
        else:
            raise AssertionError("Unexpected API call: " + endpoint)
        return subprocess.CompletedProcess(args, 0, packager._json_bytes(result), b"")

    def test_catalog_and_tags_are_one_atomic_commit_using_file_body(self):
        catalog = Path(self.temp.name) / "catalog.json"
        catalog.write_bytes(packager._json_bytes(dict(schema=1, tags=[dict(id="analog", label="$literal `metadata`")], racks=[])))
        self.client.publish_catalog(catalog)
        self.assertEqual(self.files["catalog.json"], catalog.read_bytes())
        self.assertEqual(json.loads(self.files["tags.json"])["tags"], json.loads(catalog.read_bytes())["tags"])
        self.assertEqual(self.calls[-1][0], "PATCH")
        self.assertEqual(len([c for c in self.calls if c[0] == "PATCH"]), 1)
        self.calls.clear()
        self.client.publish_catalog(catalog)
        self.assertTrue(all(method == "GET" for method, _, _ in self.calls))

    def test_failed_catalog_ref_leaves_both_previous_files_active(self):
        before = copy.deepcopy(self.files)
        catalog = Path(self.temp.name) / "catalog.json"
        catalog.write_bytes(b'{"schema":1,"tags":[],"racks":[]}')
        self.fail_ref = True
        with self.assertRaises(packager.PublicationError):
            self.client.publish_catalog(catalog)
        self.assertEqual(self.files, before)

    def test_existing_asset_hash_comes_from_downloaded_bytes(self):
        name = "ember-reed-1.0.0.airrack"
        self.client.assets = {name: dict(name=name, id=42, digest="sha256:" + "0" * 64,
            browser_download_url="https://github.com/Planetary-Records/air-rack-library/releases/download/real/" + name)}
        asset = self.client.existing_asset(name)
        self.assertEqual(asset["sha256"], hashlib.sha256(self.asset_data).hexdigest())
        self.assertEqual(asset["id"], 42)

    def test_upload_sends_exact_file_and_download_verifies_returned_asset(self):
        path = Path(self.temp.name) / "ember-reed-1.0.0.airrack"
        path.write_bytes(self.asset_data)
        self.client.assets = {}
        self.client.releases = {"ember-reed-1.0.0": 123}
        digest = hashlib.sha256(self.asset_data).hexdigest()
        asset = self.client.upload_asset(path, digest)
        self.assertEqual(asset["id"], 456)
        self.assertEqual(asset["sha256"], digest)
        self.assertEqual([method for method, _, _ in self.calls], ["POST", "GET"])
        self.assertTrue(self.calls[-1][1].endswith("/releases/assets/456"))

    def test_public_repository_with_workflow_refuses_before_mutation(self):
        self.files[".github/workflows/untrusted.yml"] = b"runner"
        with self.assertRaises(packager.PublicationError):
            self.client.existing_asset("anything")
        self.assertTrue(all(method == "GET" for method, _, _ in self.calls))

    def test_draft_release_cannot_supply_public_catalog_assets(self):
        def api(endpoint, *args, **kwargs):
            if "/releases?" in endpoint:
                return [dict(id=123, tag_name="unpublished", draft=True)]
            return []
        with mock.patch.object(self.client, "_check_repository"), mock.patch.object(self.client, "_api", side_effect=api):
            for _ in range(2):
                with self.assertRaises(packager.PublicationError):
                    self.client.existing_asset("ember-reed-1.0.0.airrack")


if __name__ == "__main__":
    unittest.main()
