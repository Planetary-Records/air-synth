"""Contributor checks: no private AIR fixtures, native binaries or third-party modules."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rack_packages", ROOT / "tools/rack_packages.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class RackAuthoringTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "racks/example-rack"
        self.source.mkdir(parents=True)
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        image = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(b"\0\0\0\0\0")) + chunk(b"IEND", b""))
        self.recipe = dict(version=2, id="c83dab6a-1c4e-48c1-83c4-80d9212ca603", revision="1",
                           name="Example rack", role="fx", units=[dict(id=0, module="drive", algorithm_revision=1)], panel={})
        (self.source / "rack.json").write_bytes(tool._json_bytes(self.recipe))
        (self.source / "screenshot.png").write_bytes(image)
        (self.source / "README.md").write_text("# Example rack\n\nAn original test recipe.\n", encoding="utf-8")
        files = []
        for path, kind in (("rack.json", "design"), ("screenshot.png", "png")):
            data = (self.source / path).read_bytes()
            files.append(dict(path=path, kind=kind, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        self.manifest = dict(schema=1, id="example-rack", version="1.0.0", design="rack.json", skin="",
                             screenshot="screenshot.png", files=files,
                             metadata=dict(design=self.recipe["id"], revision="1", canonical_sha256=files[0]["sha256"],
                                           name="Example rack", maker="Example maker", purpose="Warm drive", description="", role="fx", tags=[]),
                             capabilities=dict(airMin="0.1.0", modules=[dict(module=2, algorithmRevision=1)], nativeEditor=""))
        (self.source / "package.json").write_bytes(tool._json_bytes(self.manifest))

    def cli(self, *args, script=None):
        return subprocess.run([sys.executable, str(script or ROOT / "tools/rack_packages.py"), *map(str, args)],
                              capture_output=True, text=True, cwd=self.root)

    def test_prepare_refreshes_recipe_and_files_without_implicit_write(self):
        before = (self.source / "package.json").read_bytes()
        self.recipe.update(revision="2", name="Revised drive")
        changed = tool._json_bytes(self.recipe)
        (self.source / "rack.json").write_bytes(changed)
        self.assertTrue(callable(getattr(tool, "prepare_manifest", None)), "Single-rack manifest preparation is missing")
        prepared = tool.prepare_manifest(self.source)
        self.assertEqual((self.source / "package.json").read_bytes(), before)
        self.assertEqual(prepared["metadata"]["revision"], "2")
        self.assertEqual(prepared["metadata"]["name"], "Revised drive")
        self.assertEqual(prepared["metadata"]["canonical_sha256"], hashlib.sha256(changed).hexdigest())
        self.assertEqual(prepared["files"][0]["bytes"], len(changed))
        archive = tool.build_package(self.source, prepared, self.root / "out")
        with zipfile.ZipFile(archive) as package:
            self.assertEqual(package.read("rack.json"), changed)

    def test_build_single_rack_cli_is_deterministic_and_refuses_changed_release(self):
        result = self.cli("build-rack", "--source", self.source, "--output", self.root / "one")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.cli("build-rack", "--source", self.source, "--output", self.root / "two")
        self.assertEqual(result.returncode, 0, result.stderr)
        name = "example-rack-1.0.0.airrack"
        self.assertEqual((self.root / "one" / name).read_bytes(), (self.root / "two" / name).read_bytes())
        self.recipe["name"] = "Changed"
        (self.source / "rack.json").write_bytes(tool._json_bytes(self.recipe))
        result = self.cli("build-rack", "--source", self.source, "--output", self.root / "one")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differs", result.stderr)

    def test_explicit_prepare_updates_manifest_and_refuses_linked_input(self):
        self.recipe["revision"] = "2"
        (self.source / "rack.json").write_bytes(tool._json_bytes(self.recipe))
        result = self.cli("prepare", "--source", self.source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.source / "package.json").read_bytes())["metadata"]["revision"], "2")
        with mock.patch.object(Path, "is_symlink", lambda p: p.name == "rack.json"):
            with self.assertRaisesRegex(ValueError, "[Ll]ink"):
                tool.prepare_manifest(self.source)

    def test_export_is_standalone_allowlisted_and_binds_exact_archive(self):
        (self.source / "rack-source.json").write_text('{"private":"development map"}')
        (self.source / ".env").write_text("PRIVATE=never-export")
        output = self.root / "public"
        result = self.cli("export-source", "--source", self.source.parent, "--output", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(list(output.rglob("rack-source.json")))
        self.assertFalse(list(output.rglob(".env")))
        self.assertTrue((output / "CONTRIBUTING.md").is_file())
        self.assertTrue((output / "racks/example-rack/README.md").is_file())
        index = json.loads((output / "SOURCE-INDEX.json").read_bytes())
        self.assertEqual(index["schema"], 1)
        entry = index["releases"][0]
        archive = tool.build_package(self.source, self.manifest, self.root / "expected")
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(entry, dict(id="example-rack", version="1.0.0", sha256=digest,
                                    path="releases/example-rack/1.0.0/" + digest))
        self.assertEqual((output / entry["path"] / "rack.json").read_bytes(), (self.source / "rack.json").read_bytes())
        result = self.cli("build-rack", "--source", output / "racks/example-rack", "--output", self.root / "rebuilt",
                          script=output / "tools/rack_packages.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "rebuilt" / archive.name).read_bytes(), archive.read_bytes())

    def test_export_refuses_dirty_manifest_private_runtime_and_existing_output(self):
        result = self.cli("export-source", "--source", self.source.parent, "--output", self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("empty", result.stderr.lower())
        self.recipe["name"] = "Edited without preparing"
        (self.source / "rack.json").write_bytes(tool._json_bytes(self.recipe))
        result = self.cli("export-source", "--source", self.source.parent, "--output", self.root / "dirty")
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr.lower(), "mismatch|changed")
        self.recipe["prompt"] = "private authoring content"
        (self.source / "rack.json").write_bytes(tool._json_bytes(self.recipe))
        result = self.cli("prepare", "--source", self.source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("private", result.stderr.lower())

    def test_preview_refuses_existing_workspace_before_launching(self):
        workspace = self.root / "existing"
        workspace.mkdir()
        sentinel = workspace / "keep.txt"
        sentinel.write_text("unchanged")
        result = self.cli("preview", "--source", self.source, "--workspace", workspace,
                          "--executable", self.root / "missing.exe", "--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("workspace", result.stderr.lower())
        self.assertEqual(sentinel.read_text(), "unchanged")

    def test_release_publisher_accepts_only_named_public_source_paths(self):
        self.assertTrue(callable(getattr(tool, "public_library_path", None)), "Source-first publication allowlist is missing")
        digest = "a" * 64
        for path in ("README.md", "CONTRIBUTING.md", "SOURCE-INDEX.json", "SOURCE-CHECKSUMS.sha256",
                     "tools/rack_packages.py", "tests/test_rack_authoring.py", "racks/tags.json",
                     "racks/example-rack/package.json", "racks/example-rack/skin/parts/knob.png",
                     "releases/example-rack/1.0.0/" + digest + "/rack.json"):
            self.assertTrue(tool.public_library_path(path, "blob"), path)
        for path in (".github/workflows/run.yml", "tools/extra.py", "racks/example-rack/rack-source.json",
                     "racks/example-rack/private.json", "racks/example-rack/engine.dll",
                     "releases/example-rack/1.0.0/not-a-hash/rack.json", "racks/example-rack/../secret.json"):
            self.assertFalse(tool.public_library_path(path, "blob"), path)
        self.assertFalse(tool.public_library_path("racks/example-rack/rack.json", "commit"))

    def test_source_update_preserves_previous_release_and_rejects_version_reuse(self):
        first = self.root / "first"
        result = self.cli("export-source", "--source", self.source.parent, "--output", first)
        self.assertEqual(result.returncode, 0, result.stderr)
        original = json.loads((first / "SOURCE-INDEX.json").read_bytes())["releases"][0]
        original_bytes = (first / original["path"] / "rack.json").read_bytes()
        self.recipe["name"] = "New recipe"
        (self.source / "rack.json").write_bytes(tool._json_bytes(self.recipe))
        result = self.cli("prepare", "--source", self.source)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.cli("export-source", "--source", self.source.parent, "--output", self.root / "conflict", "--previous", first)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version", result.stderr.lower())
        manifest = json.loads((self.source / "package.json").read_bytes())
        manifest["version"] = "1.1.0"
        (self.source / "package.json").write_bytes(tool._json_bytes(manifest))
        second = self.root / "second"
        result = self.cli("export-source", "--source", self.source.parent, "--output", second, "--previous", first)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((second / original["path"] / "rack.json").read_bytes(), original_bytes)
        self.assertEqual(len(json.loads((second / "SOURCE-INDEX.json").read_bytes())["releases"]), 2)
        self.assertEqual(json.loads((second / "racks/example-rack/rack.json").read_bytes())["name"], "New recipe")

    def test_export_uses_validated_archive_snapshot_when_editor_changes_input(self):
        original = (self.source / "rack.json").read_bytes()
        build = tool.build_package
        def edit_after_validation(*args):
            archive = build(*args)
            changed = dict(self.recipe, prompt="private editor content must not enter exported source")
            (self.source / "rack.json").write_bytes(tool._json_bytes(changed))
            return archive
        output = self.root / "snapshot"
        with mock.patch.object(tool, "build_package", side_effect=edit_after_validation):
            index = tool.export_sources(self.source.parent, output)
        self.assertEqual((output / "racks/example-rack/rack.json").read_bytes(), original)
        self.assertEqual((output / index["releases"][0]["path"] / "rack.json").read_bytes(), original)

    def test_source_update_removes_obsolete_current_files_but_keeps_release(self):
        extra = self.source / "notices/obsolete.txt"
        extra.parent.mkdir()
        extra.write_bytes(b"Original optional fixture notice\n")
        self.manifest["files"].append(dict(path="notices/obsolete.txt", kind="notice", bytes=extra.stat().st_size,
                                           sha256=hashlib.sha256(extra.read_bytes()).hexdigest()))
        (self.source / "package.json").write_bytes(tool._json_bytes(self.manifest))
        first = self.root / "prior"
        old = tool.export_sources(self.source.parent, first)["releases"][0]
        self.manifest["version"] = "2.0.0"
        self.manifest["files"] = [entry for entry in self.manifest["files"] if entry["kind"] != "notice"]
        (self.source / "package.json").write_bytes(tool._json_bytes(self.manifest))
        second = self.root / "updated"
        tool.export_sources(self.source.parent, second, first)
        self.assertFalse((second / "racks/example-rack/notices/obsolete.txt").exists())
        self.assertEqual((second / old["path"] / "notices/obsolete.txt").read_bytes(), extra.read_bytes())

    def test_export_preserves_bytes_through_git_with_autocrlf_enabled(self):
        notice=self.source / "notices/original.txt"
        notice.parent.mkdir()
        notice.write_bytes(b"Original notice\r\nwith original line endings\r\n")
        self.manifest["files"].append(dict(path="notices/original.txt",kind="notice",bytes=notice.stat().st_size,
                                           sha256=hashlib.sha256(notice.read_bytes()).hexdigest()))
        (self.source / "package.json").write_bytes(tool._json_bytes(self.manifest))
        exported=self.root / "git-export"
        tool.export_sources(self.source.parent,exported)
        checkout=self.root / "checkout"
        checkout.mkdir()
        for args in (["init","--quiet"], ["-c","core.autocrlf=true","add","--all"],
                     ["-c","core.autocrlf=true","checkout-index","--all","--prefix",checkout.as_posix()+"/"]):
            result=subprocess.run(["git","-C",str(exported),*args],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
        for line in (exported / "SOURCE-CHECKSUMS.sha256").read_text().splitlines():
            digest,name=line.split("  ",1)
            self.assertEqual(hashlib.sha256((checkout/name).read_bytes()).hexdigest(),digest,name)
        result=self.cli("build-rack","--source",checkout / "racks/example-rack","--output",self.root / "roundtrip-package",
                        script=checkout / "tools/rack_packages.py")
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual((checkout / "racks/example-rack/notices/original.txt").read_bytes(),notice.read_bytes())


if __name__ == "__main__":
    unittest.main()
