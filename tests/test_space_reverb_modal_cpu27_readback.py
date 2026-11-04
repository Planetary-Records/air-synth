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
POWER='SERVICE_CPU16_POWER attempted=1 opened=1 restored=1 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=1 before_control=4 before_state=4 during_version=1 during_control=5 during_state=4 after_version=1 after_control=4 after_state=4'
SPEC=importlib.util.spec_from_file_location('cpu27_reader',ROOT/'tools/space_reverb_modal_cpu27_readback.py')
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
        self.assertFalse(attempt['eligible_interruption'])
    def test_warm_allocation_is_not_resumable(self):
        for offset in [104,112]:
            raw=bytearray(fixture(variant='partial_ok'));struct.pack_into('<Q',raw,offset,1)
            result=reader.decode_timings(raw);self.assertTrue(result['known_failure']);self.assertFalse(result['processing_pass'])
    def test_aggregate_requires_single_complete_observations_without_resume(self):
        good=[{'row':r,'complete_pass':True,'complete_observation':True,'qualified_row_pass':True,'known_failure':False,'eligible_interruption':False,'valid_condition':True,'raw':{'dispatch_misses':0}} for r in range(24)]
        interrupted={'row':0,'complete_pass':False,'known_failure':False,'eligible_interruption':True,'valid_condition':True}
        with self.assertRaises(ValueError):reader.aggregate_attempts([interrupted]+good)
        result=reader.aggregate_attempts(good);self.assertTrue(result['processing_pass']);self.assertEqual(result['status'],'PASS_PROCESSING_CONDITIONAL')
        self.assertFalse(reader.aggregate_attempts([interrupted])['resume_eligible'])
        self.assertFalse(reader.aggregate_attempts(good[:23])['processing_pass'])
    def test_failed_partial_blocks_every_later_pass(self):
        good=[{'row':r,'complete_pass':True,'known_failure':False,'eligible_interruption':False,'valid_condition':True} for r in range(24)]
        failed={'row':0,'complete_pass':False,'known_failure':True,'eligible_interruption':False,'valid_condition':True}
        result=reader.aggregate_attempts([failed]);self.assertFalse(result['processing_pass']);self.assertFalse(result['resume_eligible'])
        with self.assertRaises(ValueError):reader.aggregate_attempts([failed]+good)
        with self.assertRaises(ValueError):reader.aggregate_attempts(good+[good[0]])
    def test_attempt_conditions(self):
        partial=reader.decode_timings(fixture(variant='partial_ok'))
        eligible=reader.classify_attempt(0,3,partial,phase='workload',reason='peer_native_work',interrupted=True)
        self.assertFalse(eligible['eligible_interruption']);self.assertFalse(eligible['complete_pass'])
        unknown=reader.classify_attempt(0,3,partial,phase='workload',reason='cycle_before',interrupted=True)
        self.assertFalse(unknown['eligible_interruption']);self.assertFalse(unknown['valid_condition'])
        admission=reader.classify_attempt(0,3,None,phase='admission',reason='peer_native_work',interrupted=True)
        self.assertFalse(admission['eligible_interruption'])
        bad=reader.decode_timings(fixture(variant='partial_bad'));self.assertTrue(reader.classify_attempt(0,3,bad,phase='workload',reason='peer_native_work',interrupted=True)['known_failure'])

    def envelope(self,root,row=0,variant='healthy',raw_bytes=None):
        def write(name,data):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return hashlib.sha256(data).hexdigest().upper()
        image=write(reader.BINARY_NAME,b'synthetic envelope unit fixture only')
        launcher=write('RackSynthNativeRun.exe',b'synthetic native coordinator unit fixture')
        controller=write('controller.ps1',b'synthetic controller')
        module=Path(reader.__file__).read_bytes();reader_hash=write('reader.py',module)
        source=write('source/probes/unit.cpp',b'synthetic source')
        qualification={'status':'PASS_CPU27_TOOLING','probe_binary_sha256':image,'controller_sha256':controller,'reader_sha256':reader_hash}
        qualification_hash=write('qualification.json',(json.dumps(qualification)+'\n').encode())
        manifest={'condition':reader.CONDITION,'limits_mode':'observe_all_callbacks','supported_rows':[0,1,2,3,5,12,13,14,15,17],'rows':24,'warm_seconds':10,'measure_seconds':20,'kernel_mean_p99_fraction':.5,'copy_p99_fraction':.1,'binary_sha256':image,'controller_sha256':controller,'reader_sha256':reader_hash,'qualification_sha256':qualification_hash,'native_resource_lease':'exclusive','native_launcher_sha256':launcher,'sources':[{'path':'probes/unit.cpp','sha256':source}]}
        write('manifest.json',json.dumps(manifest).encode())
        raw_bytes=fixture(row,variant) if raw_bytes is None else raw_bytes;raw=reader.decode_timings(raw_bytes);h=raw['header']
        scalar=['schema_valid','complete','known_failure','processing_pass','workload_complete','mean_seconds','p99_seconds','max_seconds','copy_p99_seconds','cold_p99_seconds','kernel_overruns','dispatch_misses','room_copies','post_admissions','cold_installs','nine_room_callbacks','start_lateness_p99_seconds','release_finish_p99_seconds']
        metrics=' '.join(f'{k}={int(raw[k]) if isinstance(raw[k],bool) else raw[k]}' for k in scalar)
        stdout=('SERVICE_CPU10_BEGIN row=0\n'
                'SERVICE_CPU16_POWER attempted=1 opened=1 restored=1 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=1 before_control=4 before_state=4 during_version=1 during_control=5 during_state=4 after_version=1 after_control=4 after_state=4\n'
                'SERVICE_CPU10_CALLER allocation_hook_calibrated=1 opened=1 closed=1 task=Pro_Audio timer=high_resolution cadence=rational_no_skips priority_before=0 priority_during=0 priority_after=0 failure=0 platform_error=0 cleanup_error=0\n'
                'SERVICE_CPU10_PEER pid=0 image=unknown site=none win32_error=0 exit_known=0 exit_code=0 exit_query_error=0 scans=301 maximum_scan_seconds=0.001\n'
                f'SERVICE_CPU10_RESULT row={row} phase=workload modes=128 power_ok=1 limits_mode=observe_all_callbacks copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor={h["factor"]} rate={h["rate"]} block={h["block"]} factor={h["factor"]} scenario={h["scenario"]} samples={raw["samples"]} warm_completed={h["warm_completed"]} interrupted=0 interrupt_reason=none allocations={h["callback_allocations"]} frees={h["callback_frees"]} {metrics} pass={int(raw["processing_pass"])}\n'
                f'SERVICE_CPU10_FOOTER row={row} complete={int(raw["complete"])} acceptance=0\n')
        stdout=stdout.replace('SERVICE_CPU10_BEGIN row=0\n',f'SERVICE_CPU10_BEGIN row={row}\n')
        for label,key in [('SERVICE_CPU10_KERNEL_OVERRUN','kernel_overrun_records'),('SERVICE_CPU10_DISPATCH_MISS','dispatch_miss_records')]:
            stdout+=''.join(f'{label} row={row} ordinal={entry["ordinal"]}\n' for entry in raw[key])
        stdout=stdout.encode();prefix=f'row-{row:02d}-attempt-1';records=[]
        for extension,data in [('.stdout',stdout),('.stderr',b''),('.timings.bin',raw_bytes)]:
            name=prefix+extension;digest=write(name,data);records.append({'file':name,'sha256':digest,'bytes':len(data)})
        meta={'row':row,'attempt':1,'exit':0 if raw['processing_pass'] else 1,'started_utc':'2026-10-18T10:00:00Z','ended_utc':'2026-10-18T10:00:31Z','records':records}
        write(prefix+'.json',json.dumps(meta).encode())
    def test_wrong_power_with_valid_hashes_is_rejected(self):
        mutations=[(b'during_control=5',b'during_control=4'),(b'during_state=4',b'during_state=5'),
            (b'after_control=4',b'after_control=5'),(b'after_state=4',b'after_state=0'),
            (b'cleanup_error=0',b'cleanup_error=702'),(b'setup_error=0',b'setup_error=701'),
            (b'power_ok=1',b'power_ok=0'),(b'power_ok=1',b'power_ok=2'),
            (b'attempted=1',b'attempted=0'),(b'opened=1',b'opened=0'),(b'restored=1',b'restored=0'),
            (b'close1=1',b'close1=0'),(b'close2=1',b'close2=0'),
            (b'before_version=1',b'before_version=2'),(b'during_version=1',b'during_version=0'),
            (b'after_version=1',b'after_version=2'),(b'before_control=4',b'before_control=4294967296'),
            (b'during_state=4',b'during_state=-1'),(b'during_state=4',b'during_state=+4'),
            (b' after_state=4',b''),(b'attempted=1',b'attempted=1 attempted=1'),
            (b'attempted=1',b'attempted=1 extra=1'),(b'attempted=1',b'attempted=1 bare-token')]
        for before,after in mutations:
            with self.subTest(mutant=after):self.reduction_mutant(before,after)
    def test_missing_and_duplicate_power_records_are_rejected(self):
        line=POWER.encode()+b'\n'
        for replacement in (b'',line+line):
            with self.subTest(replacement=replacement):self.reduction_mutant(line,replacement)
    def test_zero_and_nonzero_original_masks_restore_exactly(self):
        for before_control,before_state in [(0,0),(1,1),(4,4),(0xfffffffe,0xffffffff)]:
            with self.subTest(masks=(before_control,before_state)),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root)
                replacement=POWER.replace('before_control=4',f'before_control={before_control}').replace('before_state=4',f'before_state={before_state}').replace('during_control=5',f'during_control={before_control|1}').replace('during_state=4',f'during_state={before_state&~1}').replace('after_control=4',f'after_control={before_control}').replace('after_state=4',f'after_state={before_state}')
                self.rebind_stdout(root,(root/'row-00-attempt-1.stdout').read_bytes().replace(POWER.encode(),replacement.encode()))
                result=reader.audit_evidence(root);self.assertEqual(result['completed_rows'],1)
    def rebind_stdout(self,root,data,row=0):
        p=root/f'row-{row:02d}-attempt-1.stdout';p.write_bytes(data);meta=root/f'row-{row:02d}-attempt-1.json';value=json.loads(meta.read_text(encoding='utf-8'))
        for entry in value['records']:
            if entry['file']==p.name:entry['sha256']=hashlib.sha256(data).hexdigest().upper();entry['bytes']=len(data)
        meta.write_text(json.dumps(value),encoding='utf-8')
    def test_untouched_power_is_only_an_unmeasured_peer_admission(self):
        untouched='SERVICE_CPU16_POWER attempted=0 opened=0 restored=0 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=0 before_control=0 before_state=0 during_version=0 during_control=0 during_state=0 after_version=0 after_control=0 after_state=0'
        result={'power_ok':'0','interrupt_reason':'peer_native_work'}
        self.assertEqual(reader._verify_power(untouched,result,'admission')['attempted'],'0')
        for changed,phase in [(result,'workload'),({'power_ok':'1','interrupt_reason':'peer_native_work'},'admission'),({'power_ok':'0','interrupt_reason':'cycle_before'},'admission')]:
            with self.subTest(result=changed,phase=phase),self.assertRaises(ValueError):reader._verify_power(untouched,changed,phase)
        with self.assertRaises(ValueError):reader._verify_power(untouched.replace('during_control=0','during_control=1'),result,'admission')
        self.assertEqual(reader._verify_power(POWER,{'power_ok':'1','interrupt_reason':'peer_native_work'},'admission')['restored'],'1')
    def test_every_power_state_field_is_required(self):
        for token in POWER.split()[1:]:
            with self.subTest(missing=token),self.assertRaises(ValueError):reader._verify_power(POWER.replace(' '+token,''),{'power_ok':'1'},'workload')
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
    def test_wrong_converter_mode_with_valid_hashes_is_rejected(self):
        self.reduction_mutant(b'converter_mode=mirrored_avx2',b'converter_mode=reference')
    def test_missing_converter_mode_with_valid_hashes_is_rejected(self):
        self.reduction_mutant(b' converter_mode=mirrored_avx2',b'')
    def test_wrong_converter_slots_with_valid_hashes_is_rejected(self):
        self.reduction_mutant(b'converter_slots=8',b'converter_slots=1')
    def test_wrong_converter_factor_with_valid_hashes_is_rejected(self):
        self.reduction_mutant(b'converter_factor=1',b'converter_factor=2')
    def test_old_sse2_cannot_qualify_avx4(self):
        self.reduction_mutant(b'converter_mode=mirrored_avx2',b'converter_mode=mirrored_sse2')
    def test_shared_lease_cannot_qualify_capacity(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root);p=root/'manifest.json';value=json.loads(p.read_text());value['native_resource_lease']='shared';p.write_text(json.dumps(value))
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_coordinator_image_is_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root);(root/'RackSynthNativeRun.exe').write_bytes(b'changed launcher')
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_evidence_hash_binding_and_incomplete_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root);result=reader.audit_evidence(root)
            self.assertEqual(result['completed_rows'],1);self.assertFalse(result['processing_pass']);self.assertFalse(result['resume_eligible'])
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

class Cpu27EnvelopeTests(unittest.TestCase):
    envelope=Cpu10ReaderTests.envelope
    rebind_stdout=Cpu10ReaderTests.rebind_stdout
    cache={}
    @classmethod
    def raw(cls,row,variant='healthy'):
        key=(row,variant)
        if key not in cls.cache:cls.cache[key]=reader.decode_timings(fixture(row,variant))
        return cls.cache[key]
    @classmethod
    def attempt(cls,row,variant='healthy',**overrides):
        raw=cls.raw(row,variant)
        args=dict(phase='workload',footer_complete=True);args.update(overrides)
        return reader.classify_attempt(row,0 if raw['processing_pass'] else 1,raw,**args)
    @classmethod
    def matrix(cls,changes=None):
        changes=changes or {}
        return [cls.attempt(row,changes.get(row,'healthy')) for row in range(24)]
    def test_original_all24_pass_remains_distinct(self):
        report=reader.aggregate_attempts(self.matrix())
        self.assertTrue(report['processing_pass']);self.assertTrue(report['qualified_processing_pass'])
        self.assertEqual(report['observed_rows'],24);self.assertEqual(report['qualified_rows'],[0,1,2,3,5,12,13,14,15,17]);self.assertFalse(report['resume_eligible'])
    def test_stress_overrun_qualifies_only_supported_envelope(self):
        report=reader.aggregate_attempts(self.matrix({4:'overrun',10:'p99',16:'cold',22:'overrun'}))
        self.assertFalse(report['processing_pass']);self.assertTrue(report['qualified_processing_pass']);self.assertEqual(report['observed_rows'],24)
        self.assertTrue(report['attempts'][4]['known_failure']);self.assertGreater(report['attempts'][4]['raw']['kernel_overruns'],0)
        self.assertGreater(report['attempts'][22]['raw']['max_seconds'],0);self.assertFalse(report['attempts'][22]['raw']['processing_pass'])
    def test_same_overrun_in_supported_case_fails(self):
        for row in [0,1,2,3,5,12,13,14,15,17]:
            with self.subTest(row=row):
                report=reader.aggregate_attempts(self.matrix({row:'overrun'}));self.assertFalse(report['qualified_processing_pass']);self.assertFalse(report['processing_pass'])
    def test_partial_matrix_cannot_qualify(self):
        for missing in [0,4,10,17,23]:
            with self.subTest(missing=missing):
                report=reader.aggregate_attempts([a for a in self.matrix() if a['row']!=missing]);self.assertFalse(report['qualified_processing_pass']);self.assertEqual(report['observed_rows'],23)
    def test_incomplete_stress_observation_cannot_qualify(self):
        report=reader.aggregate_attempts(self.matrix({4:'partial_ok'}));self.assertFalse(report['qualified_processing_pass']);self.assertLess(report['observed_rows'],24)
    def test_stress_raw_data_and_heap_and_storage_are_mandatory(self):
        mutations=[('invalid bit',lambda b:struct.pack_into('<Q',b,H.size+13*8,0)),('warm heap',lambda b:struct.pack_into('<Q',b,104,1)),('warm free',lambda b:struct.pack_into('<Q',b,112,1)),('storage zero',lambda b:struct.pack_into('<Q',b,120,0)),('storage overflow',lambda b:struct.pack_into('<Q',b,120,8*16*1024*1024+1))]
        for label,mutate in mutations:
            with self.subTest(label=label):
                raw=bytearray(fixture(4,'overrun'));mutate(raw);decoded=reader.decode_timings(raw)
                self.assertFalse(decoded['observation_valid']);self.assertFalse(decoded['timing_only_failure'])
                attempt=reader.classify_attempt(4,1,decoded,phase='workload',footer_complete=True)
                matrix=self.matrix();matrix[4]=attempt;self.assertFalse(reader.aggregate_attempts(matrix)['qualified_processing_pass'])
    def test_stress_caller_peer_footer_and_interruption_cannot_qualify(self):
        for changes in [dict(caller_ok=False),dict(peer_ok=False),dict(footer_complete=False),dict(interrupted=True),dict(reason='peer_native_work')]:
            with self.subTest(changes=changes):
                matrix=self.matrix();matrix[4]=self.attempt(4,'overrun',**changes);self.assertFalse(reader.aggregate_attempts(matrix)['qualified_processing_pass'])
    def test_replay_and_resume_are_forbidden(self):
        with self.assertRaises(ValueError):reader.aggregate_attempts(self.matrix()+[self.attempt(4)])
        partial=self.raw(4,'partial_ok');item=reader.classify_attempt(4,3,partial,phase='workload',reason='peer_native_work',interrupted=True)
        self.assertFalse(item['eligible_interruption']);self.assertFalse(reader.aggregate_attempts([item])['resume_eligible'])
    def test_full_hashed_evidence_retains_original_stress_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for row in range(24):self.envelope(root,row,'overrun' if row in (4,10,16,22) else 'healthy')
            report=reader.audit_evidence(root);self.assertTrue(report['qualified_processing_pass']);self.assertFalse(report['processing_pass']);self.assertEqual(report['observed_rows'],24)
            self.assertEqual(report['attempts'][4]['raw']['kernel_overruns'],1);self.assertGreater(report['dispatch_misses_in_observed_rows'],0)
    def test_stress_metadata_mutants_cannot_count_as_observed(self):
        mutations=[(b'limits_mode=observe_all_callbacks',b'limits_mode=stop_after_overrun'),(b' limits_mode=observe_all_callbacks',b''),(b'limits_mode=observe_all_callbacks',b'limits_mode=observe_all_callbacks limits_mode=observe_all_callbacks'),
                   (b'power_ok=1',b'power_ok=0'),(b'after_state=4',b'after_state=0'),(b'converter_mode=mirrored_avx2',b'converter_mode=mirrored_sse2'),
                   (b'allocation_hook_calibrated=1',b'allocation_hook_calibrated=0'),(b'priority_after=0',b'priority_after=1'),(b' task=Pro_Audio',b' task=Audio'),
                   (b'SERVICE_CPU10_PEER pid=0',b'SERVICE_CPU10_PEER pid=1'),(b'win32_error=0',b'win32_error=5'),(b'scans=301',b'scans=0'),(b' site=none',b' site=snapshot'),
                   (b'known_failure=1',b'known_failure=0'),(b'complete=1 acceptance=0',b'complete=0 acceptance=0')]
        for old,new in mutations:
            with self.subTest(mutation=new),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root,4,'overrun');data=(root/'row-04-attempt-1.stdout').read_bytes();self.assertIn(old,data);self.rebind_stdout(root,data.replace(old,new),4)
                try:report=reader.audit_evidence(root)
                except ValueError:continue
                self.assertFalse(report['qualified_processing_pass']);self.assertEqual(report['observed_rows'],0)
    def test_stress_header_and_record_contract_failures_cannot_be_waived(self):
        mutations=[lambda b:struct.pack_into('<Q',b,H.size+13*8,0),lambda b:struct.pack_into('<Q',b,104,1),lambda b:struct.pack_into('<Q',b,112,1),lambda b:struct.pack_into('<Q',b,120,0),lambda b:struct.pack_into('<Q',b,120,8*16*1024*1024+1)]
        for mutate in mutations:
            with self.subTest(mutation=mutate),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);data=bytearray(fixture(4,'overrun'));mutate(data);self.envelope(root,4,raw_bytes=data)
                report=reader.audit_evidence(root);self.assertEqual(report['observed_rows'],0);self.assertFalse(report['qualified_processing_pass']);self.assertEqual(report['status'],'NOT_PASS')
    def test_manifest_cannot_broaden_supported_scope_or_restore_early_stop(self):
        for field,value in [('supported_rows',list(range(24))),('supported_rows',[0,1,2,3,4,5,12,13,14,15,17]),('limits_mode','stop_after_overrun')]:
            with self.subTest(field=field,value=value),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root);path=root/'manifest.json';manifest=json.loads(path.read_text());manifest[field]=value;path.write_text(json.dumps(manifest))
                with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_selection_requires_observation_metadata_and_actual_both_factors(self):
        selection='\n'.join(f'SERVICE_CPU27_SELECTION row={row} modes=128 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor={factor} limits_mode=observe_all_callbacks rendered=1 allocations=0 frees=0 valid=1 actual_timing=0' for row,factor in [(4,1),(10,2)])
        self.assertEqual(reader.verify_selection(selection),[4,10])
        for old,new in [(' limits_mode=observe_all_callbacks',''),('limits_mode=observe_all_callbacks','limits_mode=stop_after_overrun'),('converter_factor=2','converter_factor=1'),('valid=1','valid=0'),('allocations=0','allocations=1'),('SERVICE_CPU27_SELECTION','SERVICE_CPU24_SELECTION')]:
            with self.subTest(mutant=new),self.assertRaises(ValueError):reader.verify_selection(selection.replace(old,new))
    @unittest.skipUnless(os.environ.get('AIRACK_CPU27_NATIVE_OBSERVATION'),'Native complete-overrun observation is explicit qualification only')
    def test_actual_native_complete_stress_observation(self):
        raw=reader.decode_timings(Path(os.environ['AIRACK_CPU27_NATIVE_OBSERVATION']).read_bytes())
        self.assertEqual(raw['row'],4)
        for key in ['schema_valid','complete','workload_complete','known_failure','observation_valid','timing_only_failure']:self.assertTrue(raw[key],key)
        self.assertEqual(raw['kernel_overruns'],1);self.assertFalse(raw['processing_pass']);self.assertEqual(raw['samples'],raw['header']['measure_expected'])
        matrix=self.matrix();matrix[4]=reader.classify_attempt(4,1,raw,phase='workload',footer_complete=True)
        report=reader.aggregate_attempts(matrix);self.assertTrue(report['qualified_processing_pass']);self.assertFalse(report['processing_pass']);self.assertEqual(report['observed_rows'],24)

if __name__=='__main__':unittest.main()
