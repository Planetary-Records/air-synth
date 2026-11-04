"""Portable checks of the SDK-defined private import, never native acceptance."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
SPEC=importlib.util.find_spec('saturation_gain_import')
gain=importlib.util.module_from_spec(SPEC) if SPEC else None
if SPEC: SPEC.loader.exec_module(gain)

class GainImportTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(gain,'fixed import generator must exist')
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.work=Path(self.temp.name)

    def test_fixed_data_only_assembly_and_mutations(self):
        self.assertTrue(hasattr(gain,'validate_assembly'),'fixed MASM validator required')
        raw=(ROOT/'probes/saturation/SaturationGainImport.asm').read_bytes()
        gain.validate_assembly(raw)
        for a,b in ((b'pow", 0',b'sin", 0'),(b'ALIGN(8)',b'ALIGN(4)'),
                    (b'DD IMAGEREL ag_hint',b'DQ ag_hint'),(b'DQ 0',b'DQ 1'),
                    (b' DD 0, 0',b' DD 0'),(b'READ WRITE',b'READ EXECUTE')):
            self.assertIn(a,raw)
            with self.assertRaises(ValueError):gain.validate_assembly(raw.replace(a,b,1))

    def test_fresh_temp_and_build_outputs_no_overwrite(self):
        for folder in (self.work/'fresh',ROOT/'build/gain-import-portable-test'):
            folder.mkdir(parents=True,exist_ok=True)
            path=folder/'proof.json'
            self.addCleanup(lambda p=path:p.unlink(missing_ok=True))
            target=gain.plain_output(path);target.write_bytes(b'proof')
            with self.assertRaises(ValueError):gain.plain_output(path)

    def test_source_corpus_and_repository_roots_refused_before_mutation(self):
        for path in (ROOT/'private.obj',ROOT/'tools/private.obj',ROOT/'probes/saturation/private.obj',
                     ROOT/'probes/saturation/contracts/fixtures/candidate-20261021/private.obj',
                     ROOT/'.superpowers/private.obj',ROOT/'build/../tests/private.obj'):
            with self.assertRaises(ValueError):gain.plain_output(path)
            self.assertFalse(path.exists())

    def test_redirected_existing_ancestor_refused(self):
        real=self.work/'real';real.mkdir();link=self.work/'link'
        if os.name=='nt':
            result=subprocess.run(['cmd','/c','mklink','/J',str(link),str(real)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
        else:link.symlink_to(real,target_is_directory=True)
        try:
            with self.assertRaises(ValueError):gain.plain_output(link/'nested/private.obj')
            with self.assertRaises(ValueError):gain.plain_output(link/'../unsafe-proof.json')
            self.assertFalse((real/'nested').exists())
        finally:
            if os.name=='nt':os.rmdir(link)
            else:link.unlink()

    def test_parent_traversal_never_bypasses_real_source_boundary(self):
        for path in (ROOT/'build/../docs/unsafe-proof.json', ROOT/'build/../tests/unsafe-proof.json'):
            with self.assertRaises(ValueError): gain.plain_output(path)
        self.assertTrue(hasattr(gain,'validate_assembly'),'fixed MASM source validator must exist')

    @staticmethod
    def pe():
        raw=bytearray(0x600);raw[:2]=b'MZ';struct.pack_into('<I',raw,0x3c,0x80)
        raw[0x80:0x84]=b'PE\0\0';struct.pack_into('<HHIIIHH',raw,0x84,0x8664,1,0,0,0,240,0)
        struct.pack_into('<H',raw,0x98,0x20b);struct.pack_into('<I',raw,0x98+108,16)
        struct.pack_into('<II',raw,0x98+112+8,0x1000,40)
        struct.pack_into('<II',raw,0x98+112+96,0x1080,16)
        off=0x98+240;raw[off:off+8]=b'.rdata\0\0';struct.pack_into('<IIII',raw,off+8,0x400,0x1000,0x400,0x200)
        struct.pack_into('<IIIII',raw,0x200,0x1060,0,0,0x10b0,0x1080)
        struct.pack_into('<Q',raw,0x260,0x10a0);struct.pack_into('<Q',raw,0x280,0x10a0)
        raw[0x2a0:0x2a6]=b'\0\0pow\0';dll=b'api-ms-win-crt-math-l1-1-0.dll\0';raw[0x2b0:0x2b0+len(dll)]=dll
        return bytes(raw)

    def test_pe_import_directories_and_malformed_data_refused(self):
        self.assertTrue(hasattr(gain,'inspect_pe'),'strict prelaunch PE import reader required')
        raw=self.pe();proof=gain.inspect_pe(raw)
        self.assertEqual(proof['private_pow_iat_rva'],0x1080)
        for offset,form,value in ((0x98+120,'I',0),(0x98+208,'I',0),(0x200,'I',0),
                                  (0x200+16,'I',0),(0x260,'Q',0x8000000000000001),
                                  (0x268,'Q',0x10a0),(0x288,'Q',0x10a0),(0x214,'I',0x1060)):
            altered=bytearray(raw);struct.pack_into('<'+form,altered,offset,value)
            with self.assertRaises(ValueError):gain.inspect_pe(bytes(altered))
        for bad in (raw[:100],raw.replace(b'pow\0',b'sin\0'),raw.replace(b'api-ms-win-crt',b'bad-ms-win-crt')):
            with self.assertRaises(ValueError):gain.inspect_pe(bad)

    def test_private_single_entry_descriptor_isolated_from_ordinary_pow(self):
        raw=bytearray(self.pe());struct.pack_into('<I',raw,0x98+124,60)
        struct.pack_into('<I',raw,0x98+212,32)
        struct.pack_into('<IIIII',raw,0x214,0x10d0,0,0,0x10b0,0x1090)
        struct.pack_into('<Q',raw,0x2d0,0x10e0);struct.pack_into('<Q',raw,0x290,0x10e0)
        raw[0x2e0:0x2e6]=b'\xfe\0pow\0'
        self.assertEqual(gain.inspect_pe(bytes(raw))['private_pow_iat_rva'],0x1080)

    def ordinal_pe(self):
        raw=bytearray(self.pe());struct.pack_into('<I',raw,0x98+124,60)
        struct.pack_into('<I',raw,0x98+212,32)
        struct.pack_into('<IIIII',raw,0x214,0x10d0,0,0,0x10f0,0x1090)
        struct.pack_into('<Q',raw,0x2d0,0x8000000000000073)
        struct.pack_into('<Q',raw,0x290,0x8000000000000073)
        raw[0x2f0:0x2fb]=b'WS2_32.dll\0'
        return raw

    def test_ordinary_ordinal_import_preserves_private_named_selection(self):
        try:proof=gain.inspect_pe(bytes(self.ordinal_pe()))
        except ValueError as exc:self.fail('legal ordinary ordinal import refused: '+str(exc))
        self.assertEqual(proof['private_pow_iat_rva'],0x1080)
        self.assertEqual(proof['descriptors'][1]['functions'],[dict(ordinal=115,iat_rva=0x1090)])

    def test_ordinal_reserved_bits_mismatch_termination_and_private_refused(self):
        for value in (0x8000000000010073,0x8000010000000073,0xc000000000000073):
            raw=self.ordinal_pe()
            for at in (0x2d0,0x290):struct.pack_into('<Q',raw,at,value)
            with self.assertRaises(ValueError):gain.inspect_pe(bytes(raw))
        for at,value in ((0x290,0x8000000000000074),(0x2d8,0x8000000000000073),
                         (0x298,0x8000000000000073)):
            raw=self.ordinal_pe();struct.pack_into('<Q',raw,at,value)
            with self.assertRaises(ValueError):gain.inspect_pe(bytes(raw))
        raw=bytearray(self.pe())
        for at in (0x260,0x280):struct.pack_into('<Q',raw,at,0x8000000000000073)
        with self.assertRaises(ValueError):gain.inspect_pe(bytes(raw))
        for at,value in ((0x214,0x13fc),(0x224,0x1098),(0x98+212,24)):
            raw=self.ordinal_pe();struct.pack_into('<I',raw,at,value)
            with self.assertRaises(ValueError):gain.inspect_pe(bytes(raw))

    def test_mixed_ordinary_named_and_ordinal_imports(self):
        raw=self.ordinal_pe();struct.pack_into('<I',raw,0x98+212,0x98)
        struct.pack_into('<I',raw,0x224,0x1100)
        struct.pack_into('<Q',raw,0x300,0x8000000000000073)
        for at in (0x2d8,0x308):struct.pack_into('<Q',raw,at,0x10e8)
        raw[0x2e8:0x2ee]=b'\x05\0sin\0'
        proof=gain.inspect_pe(bytes(raw))
        self.assertEqual(proof['descriptors'][1]['functions'],
                         [dict(ordinal=115,iat_rva=0x1100),dict(name='sin',hint=5,iat_rva=0x1108)])

    def test_executable_private_iat_call_is_required(self):
        self.assertTrue(hasattr(gain,'inspect_caller'),'actual linked IAT call binding required')
        raw=bytearray(self.pe());struct.pack_into('<I',raw,0x98+240+36,0x60000020)
        raw[0x300:0x306]=b'\xff\x15'+struct.pack('<i',0x1080-0x1106)
        self.assertEqual(gain.inspect_caller(bytes(raw),gain.inspect_pe(bytes(raw))),[0x1100])
        raw[0x300:0x306]=b'\x90'*6
        with self.assertRaises(ValueError):gain.inspect_caller(bytes(raw),gain.inspect_pe(bytes(raw)))

if __name__=='__main__':unittest.main()
