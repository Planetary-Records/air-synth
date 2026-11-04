import importlib.util
import hashlib
import json
import tempfile
import math
import os
from pathlib import Path
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('cpu21_reader',ROOT/'tools/space_reverb_modal_cpu21_readback.py')
reader=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(reader)
H=struct.Struct('<Q6I12Q');R=struct.Struct('<14Q')
def bits(value):return struct.unpack('<Q',struct.pack('<d',value))[0]
def fixture(row=0,variant='healthy'):
    index=row%12;rate=[44100,48000,96000][index//2%3];block=[64,512][index%2];factor=index//6+1;scene=row//12;tps=10**9
    warm=(10*rate+block-1)//block;total=(20*rate+block-1)//block;start=10**9;warm_end=start+(warm+1)*tps*block//rate;anchor=warm_end+10**6;period=tps*block//rate;previous=0;records=[]
    for i in range(total):
        release=anchor+(i+1)*tps*block//rate;begin=max(release+period//100+(2*period if variant=='late' and i==100 else 0),previous)
        duration=period//4
        if variant=='overrun' and i==10:duration=2*period
        if variant=='p99' and i<total//50:duration=period*3//5
        if variant=='mean':duration=period*9//10 if i<total//100 else period*499//1000
        maximum=9 if i%16<8 else 8;copy=int(i%16==0);post=int(i%16==8);cold=0
        if scene:maximum=8;copy=post=0;cold=int((warm+i)%((rate+4*block-1)//(4*block))==0)
        if variant=='no_work':maximum=8;copy=post=cold=0
        if variant=='cold' and cold:duration=period*3//5
        begin_finish=begin+(period*11//100 if variant=='copy' and copy else period//20);finish=begin+duration;previous=finish
        records.append([release,begin,begin_finish,finish,5678,copy,post,cold,maximum,0,0,bits(.1),bits(i/1024),1])
    if variant.startswith('partial'):
        records=records[:20]
        if variant=='partial_bad':
            records[10][3]=records[10][1]+2*period
            for i in range(11,len(records)):
                delta=max(0,records[i-1][3]-records[i][1]);records[i][1]+=delta;records[i][2]+=delta;records[i][3]+=delta
    end=records[-1][3] if variant.startswith('partial') else anchor+(total+1)*tps*block//rate
    header=[0x3155504356525353,1,row,rate,block,factor,scene,warm,total,tps,anchor,start,warm_end,warm,len(records),end,0,0,32*1024*1024]
    return H.pack(*header)+b''.join(R.pack(*r) for r in records)

class Cpu10ReaderTests(unittest.TestCase):
    def test_all_twenty_four_rows(self):
        for row in range(24):
            with self.subTest(row=row):
                result=reader.decode_timings(fixture(row));self.assertTrue(result['processing_pass']);self.assertEqual(result['row'],row);self.assertEqual(result['dispatch_misses'],0)
    def test_late_dispatch_is_separate(self):
        result=reader.decode_timings(fixture(variant='late'));self.assertTrue(result['processing_pass']);self.assertFalse(result['known_failure']);self.assertGreater(result['dispatch_misses'],0)
    def test_processing_budget_mutants(self):
        for variant in ['overrun','p99','mean','copy','no_work']:
            with self.subTest(variant=variant):
                result=reader.decode_timings(fixture(variant=variant));self.assertFalse(result['processing_pass']);self.assertTrue(result['known_failure'])
        result=reader.decode_timings(fixture(12,'cold'));self.assertFalse(result['processing_pass']);self.assertTrue(result['known_failure'])
    def test_partial_classification(self):
        result=reader.decode_timings(fixture(variant='partial_ok'));self.assertFalse(result['complete']);self.assertFalse(result['known_failure'])
        result=reader.decode_timings(fixture(variant='partial_bad'));self.assertFalse(result['complete']);self.assertTrue(result['known_failure']);self.assertEqual(result['kernel_overruns'],1)
    def test_schema_rejects_malformed(self):
        original=fixture()
        corrupt=bytearray(original);struct.pack_into('<I',corrupt,8,2)
        with self.assertRaises(ValueError):reader.decode_timings(corrupt)
        with self.assertRaises(ValueError):reader.decode_timings(original[:-1])
        corrupt=bytearray(original);struct.pack_into('<Q',corrupt,128+11*8,bits(math.nan))
        with self.assertRaises(ValueError):reader.decode_timings(corrupt)
        corrupt=bytearray(original);struct.pack_into('<Q',corrupt,128+6*8,1)
        with self.assertRaises(ValueError):reader.decode_timings(corrupt)
        corrupt=bytearray(original);struct.pack_into('<Q',corrupt,96,2**64-1)
        with self.assertRaises(ValueError):reader.decode_timings(corrupt)
    def test_warm_only_peer_interruption(self):
        data=fixture();header=list(H.unpack_from(data));header[10]=0;header[12]=header[11]+10**9;header[13]=100;header[14]=0;header[15]=0
        result=reader.decode_timings(H.pack(*header));self.assertFalse(result['complete']);self.assertFalse(result['known_failure'])
        attempt=reader.classify_attempt(0,3,result,phase='workload',reason='peer_native_work',interrupted=True)
        self.assertTrue(attempt['eligible_interruption'])
    def test_warm_allocation_is_not_resumable(self):
        for offset in [104,112]:
            raw=bytearray(fixture(variant='partial_ok'));struct.pack_into('<Q',raw,offset,1)
            result=reader.decode_timings(raw);self.assertTrue(result['known_failure']);self.assertFalse(result['processing_pass'])
    def test_aggregate_ignores_eligible_interruption(self):
        good=[{'row':r,'complete_pass':True,'known_failure':False,'eligible_interruption':False,'valid_condition':True} for r in range(24)]
        interrupted={'row':0,'complete_pass':False,'known_failure':False,'eligible_interruption':True,'valid_condition':True}
        result=reader.aggregate_attempts([interrupted]+good);self.assertTrue(result['processing_pass']);self.assertEqual(result['status'],'PASS_PROCESSING_CONDITIONAL')
        self.assertTrue(reader.aggregate_attempts([interrupted])['resume_eligible'])
        self.assertFalse(reader.aggregate_attempts(good[:23])['processing_pass'])
    def test_failed_partial_blocks_every_later_pass(self):
        good=[{'row':r,'complete_pass':True,'known_failure':False,'eligible_interruption':False,'valid_condition':True} for r in range(24)]
        failed={'row':0,'complete_pass':False,'known_failure':True,'eligible_interruption':False,'valid_condition':True}
        result=reader.aggregate_attempts([failed]+good);self.assertFalse(result['processing_pass']);self.assertFalse(result['resume_eligible'])
        with self.assertRaises(ValueError):reader.aggregate_attempts(good+[good[0]])
    def test_attempt_conditions(self):
        partial=reader.decode_timings(fixture(variant='partial_ok'))
        eligible=reader.classify_attempt(0,3,partial,phase='workload',reason='peer_native_work',interrupted=True)
        self.assertTrue(eligible['eligible_interruption']);self.assertFalse(eligible['complete_pass'])
        unknown=reader.classify_attempt(0,3,partial,phase='workload',reason='cycle_before',interrupted=True)
        self.assertFalse(unknown['eligible_interruption']);self.assertFalse(unknown['valid_condition'])
        admission=reader.classify_attempt(0,3,None,phase='admission',reason='peer_native_work',interrupted=True)
        self.assertTrue(admission['eligible_interruption'])
        bad=reader.decode_timings(fixture(variant='partial_bad'));self.assertTrue(reader.classify_attempt(0,3,bad,phase='workload',reason='peer_native_work',interrupted=True)['known_failure'])

    def envelope(self,root):
        def write(name,data):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return hashlib.sha256(data).hexdigest().upper()
        image=write(reader.BINARY_NAME,b'synthetic envelope unit fixture only')
        controller=write('controller.ps1',b'synthetic controller')
        module=Path(reader.__file__).read_bytes();reader_hash=write('reader.py',module)
        source=write('source/probes/unit.cpp',b'synthetic source')
        qualification={'status':'PASS_CPU21_TOOLING','probe_binary_sha256':image,'controller_sha256':controller,'reader_sha256':reader_hash}
        qualification_hash=write('qualification.json',(json.dumps(qualification)+'\n').encode())
        manifest={'condition':reader.CONDITION,'rows':24,'warm_seconds':10,'measure_seconds':20,'kernel_mean_p99_fraction':.5,'copy_p99_fraction':.1,'binary_sha256':image,'controller_sha256':controller,'reader_sha256':reader_hash,'qualification_sha256':qualification_hash,'sources':[{'path':'probes/unit.cpp','sha256':source}]}
        write('manifest.json',json.dumps(manifest).encode())
        raw_bytes=fixture();raw=reader.decode_timings(raw_bytes);h=raw['header']
        scalar=['schema_valid','complete','known_failure','processing_pass','workload_complete','mean_seconds','p99_seconds','max_seconds','copy_p99_seconds','cold_p99_seconds','kernel_overruns','dispatch_misses','room_copies','post_admissions','cold_installs','nine_room_callbacks','start_lateness_p99_seconds','release_finish_p99_seconds']
        metrics=' '.join(f'{k}={int(raw[k]) if isinstance(raw[k],bool) else raw[k]}' for k in scalar)
        stdout=('SERVICE_CPU10_BEGIN row=0\n'
                'SERVICE_CPU10_CALLER allocation_hook_calibrated=1 opened=1 closed=1 task=Pro_Audio timer=high_resolution cadence=rational_no_skips priority_before=0 priority_during=0 priority_after=0 failure=0 platform_error=0 cleanup_error=0\n'
                'SERVICE_CPU10_PEER pid=0 image=unknown site=none win32_error=0 exit_known=0 exit_code=0 exit_query_error=0 scans=301 maximum_scan_seconds=0.001\n'
                f'SERVICE_CPU10_RESULT row=0 phase=workload modes=128 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo rate={h["rate"]} block={h["block"]} factor={h["factor"]} scenario={h["scenario"]} samples={raw["samples"]} warm_completed={h["warm_completed"]} interrupted=0 interrupt_reason=none allocations=0 frees=0 {metrics} pass=1\n'
                'SERVICE_CPU10_FOOTER row=0 complete=1 acceptance=0\n').encode()
        prefix='row-00-attempt-1';records=[]
        for extension,data in [('.stdout',stdout),('.stderr',b''),('.timings.bin',raw_bytes)]:
            name=prefix+extension;digest=write(name,data);records.append({'file':name,'sha256':digest,'bytes':len(data)})
        meta={'row':0,'attempt':1,'exit':0,'started_utc':'2026-10-18T10:00:00Z','ended_utc':'2026-10-18T10:00:31Z','records':records}
        write(prefix+'.json',json.dumps(meta).encode())
    def test_wrong_density_with_valid_hashes_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root)
            p=root/'row-00-attempt-1.stdout';data=p.read_bytes().replace(b'modes=128',b'modes=512');p.write_bytes(data)
            meta=root/'row-00-attempt-1.json';value=json.loads(meta.read_text(encoding='utf-8'))
            for entry in value['records']:
                if entry['file']==p.name:entry['sha256']=hashlib.sha256(data).hexdigest().upper();entry['bytes']=len(data)
            meta.write_text(json.dumps(value),encoding='utf-8')
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_wrong_copy_mode_with_valid_hashes_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root)
            p=root/'row-00-attempt-1.stdout';data=p.read_bytes().replace(b'copy_mode=prepared',b'copy_mode=validated');p.write_bytes(data)
            meta=root/'row-00-attempt-1.json';value=json.loads(meta.read_text(encoding='utf-8'))
            for entry in value['records']:
                if entry['file']==p.name:entry['sha256']=hashlib.sha256(data).hexdigest().upper();entry['bytes']=len(data)
            meta.write_text(json.dumps(value),encoding='utf-8')
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_wrong_scatter_mode_with_valid_hashes_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root)
            p=root/'row-00-attempt-1.stdout';data=p.read_bytes().replace(b'scatter_mode=blocked',b'scatter_mode=scalar_reference');p.write_bytes(data)
            meta=root/'row-00-attempt-1.json';value=json.loads(meta.read_text(encoding='utf-8'))
            for entry in value['records']:
                if entry['file']==p.name:entry['sha256']=hashlib.sha256(data).hexdigest().upper();entry['bytes']=len(data)
            meta.write_text(json.dumps(value),encoding='utf-8')
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_wrong_reduction_mode_with_valid_hashes_is_rejected(self):
        self.reduction_mutant(b'reduction_mode=paired_stereo',b'reduction_mode=reference')
    def test_missing_reduction_mode_with_valid_hashes_is_rejected(self):
        self.reduction_mutant(b' reduction_mode=paired_stereo',b'')
    def reduction_mutant(self,old,new):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root)
            p=root/'row-00-attempt-1.stdout';original=p.read_bytes();self.assertIn(old,original)
            data=original.replace(old,new);p.write_bytes(data)
            meta=root/'row-00-attempt-1.json';value=json.loads(meta.read_text(encoding='utf-8'))
            for entry in value['records']:
                if entry['file']==p.name:entry['sha256']=hashlib.sha256(data).hexdigest().upper();entry['bytes']=len(data)
            meta.write_text(json.dumps(value),encoding='utf-8')
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_evidence_hash_binding_and_incomplete_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root);result=reader.audit_evidence(root)
            self.assertEqual(result['completed_rows'],1);self.assertFalse(result['processing_pass']);self.assertTrue(result['resume_eligible'])
            (root/'row-00-attempt-1.stdout').write_bytes(b'corrupted')
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_orphaned_native_attempt_cannot_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root);(root/'row-01-attempt-1.stdout').write_bytes(b'unclassified')
            with self.assertRaises(ValueError):reader.audit_evidence(root)

    @unittest.skipUnless(os.environ.get('AIRACK_CPU10_NATIVE_FIXTURES'),'Native fixture cross-check is explicit qualification only')
    def test_actual_native_serialized_fixtures(self):
        base=Path(os.environ['AIRACK_CPU10_NATIVE_FIXTURES'])
        for variant in range(6):
            result=reader.decode_timings((base/f'fixture-{variant}.timings.bin').read_bytes())
            expected=dict(x.split('=') for x in (base/f'fixture-{variant}.expected.txt').read_text(encoding='utf-8').split()[1:])
            for field in ['schema_valid','complete','known_failure','processing_pass','workload_complete','kernel_overruns','dispatch_misses','room_copies','post_admissions','cold_installs','nine_room_callbacks']:
                self.assertEqual(int(result[field]),int(expected[field]),(variant,field))
            for field in ['mean_seconds','p99_seconds','max_seconds','copy_p99_seconds','cold_p99_seconds','start_lateness_p99_seconds','release_finish_p99_seconds']:
                self.assertAlmostEqual(result[field],float(expected[field]),delta=1e-12,msg=f'{variant}:{field}')

if __name__=='__main__':unittest.main()
