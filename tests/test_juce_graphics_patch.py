"""Check the graphics-copy boundary: immutable input, idempotence and fail-closed preimage."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")
MODULES = ROOT / ".tools" / "juce" / "modules"
NATIVE = Path("juce_graphics/native/juce_Direct2DGraphicsContextImpl_windows.cpp")
PNG = Path("juce_graphics/image_formats/juce_PNGLoader.cpp")
EXPLICIT = False


class GraphicsPatchTest(unittest.TestCase):
    def test_copy_is_guarded_immutable_and_idempotent(self):
        if CMAKE is None or not all((MODULES / path).is_file() for path in (NATIVE, PNG)):
            if EXPLICIT:
                self.fail("The dedicated check requires its explicit CMake and JUCE inputs")
            self.skipTest("Optional local CMake/JUCE unavailable; dedicated CTest supplies explicit inputs")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / "input", root / "output"
            original = (MODULES / NATIVE).read_bytes()
            (source / NATIVE).parent.mkdir(parents=True)
            (source / NATIVE).write_bytes(original)
            png_original = (MODULES / PNG).read_bytes()
            (source / PNG).parent.mkdir(parents=True)
            (source / PNG).write_bytes(png_original)
            script = root / "patch.cmake"
            script.write_text(f'include("{(ROOT / "tools/JuceGraphics.cmake").as_posix()}")\n'
                              f'racksynth_prepare_juce_graphics("{source.as_posix()}" "{output.as_posix()}")\n')
            def apply():
                return subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True)
            result = apply()
            self.assertEqual(result.returncode, 0, result.stderr)
            patched = (output / NATIVE).read_bytes()
            self.assertNotEqual(patched, original)
            png_patched = (output / PNG).read_bytes()
            self.assertNotEqual(png_patched, png_original)
            self.assertIn(b"longjmp (*static_cast<jmp_buf*> (png_get_error_ptr (p)), 1);", png_patched)
            png_timestamp = (output / PNG).stat().st_mtime_ns
            timestamp = (output / NATIVE).stat().st_mtime_ns
            result = apply()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / NATIVE).read_bytes(), patched)
            self.assertEqual((output / NATIVE).stat().st_mtime_ns, timestamp)
            self.assertEqual((source / NATIVE).read_bytes(), original)
            self.assertEqual((output / PNG).read_bytes(), png_patched)
            self.assertEqual((output / PNG).stat().st_mtime_ns, png_timestamp)
            self.assertEqual((source / PNG).read_bytes(), png_original)
            (source / NATIVE).write_bytes(original + b"\n// unknown input\n")
            result = apply()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unsupported JUCE graphics layer preimage", result.stderr)
            self.assertEqual((output / NATIVE).read_bytes(), patched)
            self.assertEqual((output / PNG).read_bytes(), png_patched)
            (source / NATIVE).write_bytes(original)
            (source / PNG).write_bytes(png_original + b"\n// unknown input\n")
            result = apply()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unsupported JUCE PNG loader preimage", result.stderr)
            self.assertEqual((output / NATIVE).read_bytes(), patched)
            self.assertEqual((output / PNG).read_bytes(), png_patched)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--juce-modules", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    CMAKE, MODULES = args.cmake, args.juce_modules
    EXPLICIT = True
    unittest.main(argv=[__file__, *remaining])
