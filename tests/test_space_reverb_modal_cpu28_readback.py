import importlib.util
import hashlib
import json
import tempfile
import math
import os
from pathlib import Path
import struct
import unittest
import io
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
POWER='SERVICE_CPU16_POWER attempted=1 opened=1 restored=1 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=1 before_control=4 before_state=4 during_version=1 during_control=5 during_state=4 after_version=1 after_control=4 after_state=4'
CPU_SETS='\n'.join(f'SERVICE_CPU28_CPU id={10+i} group=0 index={i} core={i} efficiency={efficiency} flags={flags}' for i,efficiency,flags in [(0,255,0),(1,2,0),(2,2,6),(3,1,0)])
AFFINITY='SERVICE_CPU28_AFFINITY attempted=1 opened=1 restored=1 close1=1 close2=1 setup_error=0 cleanup_error=0 topology_error=0 group_count=1 before_ok=1 during_ok=1 after_ok=1 before_group=0 before_mask=15 during_group=0 during_mask=2 after_group=0 after_mask=15 selected_id=11 selected_group=0 selected_index=1 selected_core=1 selected_efficiency=2 selected_flags=0 eligible_mask=14 performance_mask=6'
SPEC=importlib.util.spec_from_file_location('cpu28_reader',ROOT/'tools/space_reverb_modal_cpu28_readback.py')
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
    def local_handoff_envelope(self,root):
        self.stable_timer_envelope(root)
        condition='complete-modal128-periodic-proaudio-processing33-local-preemptible-helper-all-deadlines-v1'
        manifest=json.loads((root/'manifest.json').read_text());manifest['condition']=condition
        q=json.loads((root/'qualification.json').read_text());q.update(condition=condition,status='PASS_CPU33_TOOLING')
        qb=(json.dumps(q)+'\n').encode();(root/'qualification.json').write_bytes(qb)
        manifest['qualification_sha256']=hashlib.sha256(qb).hexdigest().upper();(root/'manifest.json').write_text(json.dumps(manifest))
        data=(root/'row-00-attempt-1.stdout').read_bytes().replace(b'helper_minimum_sleep_100ns=10000',b'helper_minimum_sleep_100ns=10000 helper_policy=local_preemptible_non_mmcss15 caller_precise_wait_ns=150000')
        self.rebind_stdout(root,data)
    def test_local_handoff_requires_distinct_condition_and_helper_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.local_handoff_envelope(root)
            report=reader.audit_evidence(root)
            self.assertEqual(report['condition'],'complete-modal128-periodic-proaudio-processing33-local-preemptible-helper-all-deadlines-v1')
            self.assertEqual(report['completed_rows'],1)
    def test_local_handoff_rejects_wrong_or_missing_helper_and_caller_guard(self):
        for old,new in [(b'helper_policy=local_preemptible_non_mmcss15',b''),(b'helper_policy=local_preemptible_non_mmcss15',b'helper_policy=remote_mmcss_high'),(b'caller_precise_wait_ns=150000',b'caller_precise_wait_ns=300000'),(b'helper_minimum_sleep_100ns=10000',b'helper_minimum_sleep_100ns=9999')]:
            with self.subTest(new=new),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);self.local_handoff_envelope(root);self.rebind_stdout(root,(root/'row-00-attempt-1.stdout').read_bytes().replace(old,new))
                with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_local_handoff_cannot_relabel_prior_timer_packet(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.local_handoff_envelope(root);manifest=json.loads((root/'manifest.json').read_text());manifest['condition']=reader.STABLE_CONDITION;(root/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def stable_timer_envelope(self,root):
        condition='complete-modal128-periodic-proaudio-processing32-stable-caller-honored-timers-all-deadlines-v1'
        mode='single_fast_stable_reserved01_current_thread'
        self.envelope(root,condition=condition)
        path=root/'manifest.json';manifest=json.loads(path.read_text());manifest['affinity_mode']=mode;path.write_text(json.dumps(manifest))
        data=(root/'row-00-attempt-1.stdout').read_bytes()
        for old,new in [(reader.AFFINITY_MODE,mode),('during_state=4','during_state=0'),
                        ('id=13 group=0 index=3 core=3 efficiency=1 flags=0','id=13 group=0 index=3 core=3 efficiency=2 flags=1'),
                        ('during_mask=2 ','during_mask=8 '),('selected_id=11','selected_id=13'),('selected_index=1','selected_index=3'),
                        ('selected_core=1','selected_core=3'),('selected_flags=0','selected_flags=1'),
                        ('eligible_mask=14 performance_mask=6','eligible_mask=12 performance_mask=12'),
                        ('cadence=rational_no_skips','cadence=rational_no_skips helper_minimum_sleep_100ns=10000')]:
            data=data.replace(old.encode(),new.encode())
        self.rebind_stdout(root,data)
    def test_stable_timer_envelope_requires_fixed_highest_fast_cpu_and_sleep_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.stable_timer_envelope(root);report=reader.audit_evidence(root)
            self.assertEqual(report['completed_rows'],1)
            self.assertEqual(report['condition'],'complete-modal128-periodic-proaudio-processing32-stable-caller-honored-timers-all-deadlines-v1')
    def test_stable_timer_rejects_missing_or_wrong_sleep_policy(self):
        for new in [b'',b' helper_minimum_sleep_100ns=9999',b' helper_minimum_sleep_100ns=0']:
            with self.subTest(marker=new),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);self.stable_timer_envelope(root)
                data=(root/'row-00-attempt-1.stdout').read_bytes().replace(b' helper_minimum_sleep_100ns=10000',new)
                self.rebind_stdout(root,data)
                with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_stable_timer_selection_ignores_temporary_parking_and_rejects_lower_core(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.stable_timer_envelope(root)
            data=(root/'row-00-attempt-1.stdout').read_bytes().replace(b'efficiency=2 flags=1',b'efficiency=2 flags=0').replace(b'selected_flags=1',b'selected_flags=0')
            self.rebind_stdout(root,data);self.assertEqual(reader.audit_evidence(root)['completed_rows'],1)
            for old,new in [(b'during_mask=8 ',b'during_mask=4 '),(b'selected_id=13',b'selected_id=12'),(b'selected_index=3',b'selected_index=2'),(b'selected_core=3',b'selected_core=2'),(b'selected_flags=0',b'selected_flags=6')]:data=data.replace(old,new)
            self.rebind_stdout(root,data)
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_honored_timer_power_policy_does_not_relabel_legacy_power(self):
        honored=POWER.replace('during_state=4','during_state=0')
        self.assertEqual(reader._verify_power(honored,{'power_ok':'1'},'workload',5)['during_state'],'0')
        self.assertEqual(reader._verify_power(POWER,{'power_ok':'1'},'workload')['during_state'],'4')
        with self.assertRaises(ValueError):reader._verify_power(POWER,{'power_ok':'1'},'workload',5)
        with self.assertRaises(ValueError):reader._verify_power(honored,{'power_ok':'1'},'workload')
    def test_honored_timer_power_preserves_unrelated_bits_and_exact_restoration(self):
        text=POWER.replace('before_control=4','before_control=12').replace('before_state=4','before_state=12').replace('during_control=5','during_control=13').replace('during_state=4','during_state=8').replace('after_control=4','after_control=12').replace('after_state=4','after_state=12')
        self.assertEqual(reader._verify_power(text,{'power_ok':'1'},'workload',5)['during_state'],'8')
        with self.assertRaises(ValueError):reader._verify_power(text.replace('after_state=12','after_state=4'),{'power_ok':'1'},'workload',5)
    def test_honored_timer_envelope_requires_actual_new_power_and_reserved_placement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.envelope(root,condition=reader.TIMER_CONDITION)
            with self.assertRaises(ValueError):reader.audit_evidence(root)
            path=root/'manifest.json';manifest=json.loads(path.read_text());manifest['affinity_mode']=reader.RESERVED_AFFINITY_MODE;path.write_text(json.dumps(manifest))
            data=(root/'row-00-attempt-1.stdout').read_bytes()
            for old,new in [(reader.AFFINITY_MODE,reader.RESERVED_AFFINITY_MODE),('during_mask=2 ','during_mask=4 '),('selected_id=11','selected_id=12'),('selected_index=1','selected_index=2'),('selected_core=1','selected_core=2'),('selected_flags=0','selected_flags=6'),('eligible_mask=14 performance_mask=6','eligible_mask=12 performance_mask=4')]:data=data.replace(old.encode(),new.encode())
            self.rebind_stdout(root,data)
            with self.assertRaises(ValueError):reader.audit_evidence(root)
            self.rebind_stdout(root,data.replace(b'during_state=4',b'during_state=0'))
            report=reader.audit_evidence(root)
            self.assertEqual(report['condition'],reader.TIMER_CONDITION)
            self.assertEqual(report['affinity_mode'],reader.RESERVED_AFFINITY_MODE)
    def test_reserved_caller_policy_is_recomputed_from_physical_topology(self):
        cpus='\n'.join(f'SERVICE_CPU28_CPU id={10+i} group=0 index={i} core={i if i<6 else i-6} efficiency=2 flags=0' for i in range(8))
        affinity=AFFINITY.replace('mask=15','mask=255').replace('during_mask=2 ','during_mask=4 ').replace('selected_id=11','selected_id=12').replace('selected_index=1','selected_index=2').replace('selected_core=1','selected_core=2').replace('eligible_mask=14 performance_mask=6','eligible_mask=60 performance_mask=60')
        result={'affinity_mode':'single_fast_reserved01_current_thread','affinity_ok':'1'}
        for topology in (cpus,'\n'.join(reversed(cpus.splitlines()))):
            actual=reader._verify_affinity(topology+'\n'+affinity,result,'workload')
            self.assertEqual(actual['selected_index'],'2')
        for old,new in [('during_mask=4','during_mask=2'),('eligible_mask=60','eligible_mask=252'),('selected_index=2','selected_index=6'),('after_mask=255','after_mask=254')]:
            with self.assertRaises(ValueError):reader._verify_affinity(cpus+'\n'+affinity.replace(old,new),result,'workload')
    def test_reserved_caller_cannot_be_claimed_by_historical_envelope(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.envelope(root)
            manifest=json.loads((root/'manifest.json').read_text())
            manifest['affinity_mode']='single_fast_reserved01_current_thread'
            (root/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_reserved_envelope_requires_matching_new_condition_and_actual_placement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.envelope(root,condition=reader.RESERVED_CONDITION)
            with self.assertRaises(ValueError):reader.audit_evidence(root)
            path=root/'manifest.json';manifest=json.loads(path.read_text());manifest['affinity_mode']=reader.RESERVED_AFFINITY_MODE;path.write_text(json.dumps(manifest))
            data=(root/'row-00-attempt-1.stdout').read_bytes()
            for old,new in [(reader.AFFINITY_MODE,reader.RESERVED_AFFINITY_MODE),('during_mask=2 ','during_mask=4 '),('selected_id=11','selected_id=12'),('selected_index=1','selected_index=2'),('selected_core=1','selected_core=2'),('selected_flags=0','selected_flags=6'),('eligible_mask=14 performance_mask=6','eligible_mask=12 performance_mask=4')]:data=data.replace(old.encode(),new.encode())
            self.rebind_stdout(root,data)
            report=reader.audit_evidence(root)
            self.assertEqual(report['affinity_mode'],reader.RESERVED_AFFINITY_MODE)
            self.assertEqual(report['condition'],reader.RESERVED_CONDITION)
            self.assertEqual(report['completed_rows'],1)
    def test_strict_all_deadlines_accepts_only_complete_zero_miss_matrix(self):
        rows=[{'row':r,'complete_pass':True,'complete_observation':True,'qualified_row_pass':True,'known_failure':False,'eligible_interruption':False,'valid_condition':True,'raw':{'dispatch_misses':0}} for r in range(24)]
        report=reader.aggregate_attempts(rows)
        self.assertTrue(reader.require_all_deadlines(report))
        self.assertEqual(report['status'],'PASS_ALL24_DEADLINES')
        self.assertFalse(reader.require_all_deadlines(reader.aggregate_attempts(rows[:23])))
        rows[4]['complete_pass']=False
        self.assertFalse(reader.require_all_deadlines(reader.aggregate_attempts(rows)))
    def test_strict_all_deadlines_rejects_real_late_raw_processing_pass(self):
        rows=[{'row':r,'complete_pass':True,'complete_observation':True,'qualified_row_pass':True,'known_failure':False,'eligible_interruption':False,'valid_condition':True,'raw':{'dispatch_misses':0}} for r in range(24)]
        rows[4]['raw']=reader.decode_timings(fixture(4,'late'))
        report=reader.aggregate_attempts(rows)
        self.assertTrue(report['processing_pass'])
        self.assertGreater(report['dispatch_misses_in_observed_rows'],0)
        self.assertFalse(reader.require_all_deadlines(report))
        self.assertEqual(report['status'],'NOT_PASS_ALL24_DEADLINES')
    def test_strict_all_deadlines_cli_cannot_return_processing_only_success(self):
        report={'processing_pass':True,'qualified_processing_pass':True,'status':'PASS_PROCESSING_CONDITIONAL','completed_rows':24,'observed_rows':24,'dispatch_misses_in_observed_rows':1}
        with patch.object(reader,'audit_evidence',return_value=report),patch('sys.argv',['reader','unused','--require-all-deadlines']),patch('sys.stdout',new_callable=io.StringIO):
            self.assertEqual(reader.main(),1)
        self.assertFalse(report['all_deadlines_pass'])
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

    def envelope(self,root,row=0,variant='healthy',raw_bytes=None,condition=None):
        def write(name,data):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return hashlib.sha256(data).hexdigest().upper()
        image=write(reader.BINARY_NAME,b'synthetic envelope unit fixture only')
        launcher=write('RackSynthNativeRun.exe',b'synthetic native coordinator unit fixture')
        controller=write('controller.ps1',b'synthetic controller')
        module=Path(reader.__file__).read_bytes();reader_hash=write('reader.py',module)
        condition=reader.CONDITION if condition is None else condition
        source_path='src/dsp/space/runtime/ModalOuter.cpp'
        source=write('source/'+source_path,b'synthetic changed ModalOuter source')
        qualification={'status':reader.QUALIFICATION_STATUS.get(condition,'UNKNOWN'),'condition':condition,'probe_binary_sha256':image,'controller_sha256':controller,'reader_sha256':reader_hash}
        qualification_hash=write('qualification.json',(json.dumps(qualification)+'\n').encode())
        manifest={'condition':condition,'limits_mode':'observe_all_callbacks','affinity_mode':'single_fast_nonzero_current_thread','supported_rows':[0,1,2,3,5,12,13,14,15,17],'rows':24,'warm_seconds':10,'measure_seconds':20,'kernel_mean_p99_fraction':.5,'copy_p99_fraction':.1,'binary_sha256':image,'controller_sha256':controller,'reader_sha256':reader_hash,'qualification_sha256':qualification_hash,'native_resource_lease':'exclusive','native_launcher_sha256':launcher,'sources':[{'path':source_path,'sha256':source}]}
        write('manifest.json',json.dumps(manifest).encode())
        raw_bytes=fixture(row,variant) if raw_bytes is None else raw_bytes;raw=reader.decode_timings(raw_bytes);h=raw['header']
        scalar=['schema_valid','complete','known_failure','processing_pass','workload_complete','mean_seconds','p99_seconds','max_seconds','copy_p99_seconds','cold_p99_seconds','kernel_overruns','dispatch_misses','room_copies','post_admissions','cold_installs','nine_room_callbacks','start_lateness_p99_seconds','release_finish_p99_seconds']
        metrics=' '.join(f'{k}={int(raw[k]) if isinstance(raw[k],bool) else raw[k]}' for k in scalar)
        stdout=(CPU_SETS+'\n'+AFFINITY+'\n'+'SERVICE_CPU10_BEGIN row=0\n'
                'SERVICE_CPU16_POWER attempted=1 opened=1 restored=1 close1=1 close2=1 setup_error=0 cleanup_error=0 before_version=1 before_control=4 before_state=4 during_version=1 during_control=5 during_state=4 after_version=1 after_control=4 after_state=4\n'
                'SERVICE_CPU10_CALLER allocation_hook_calibrated=1 opened=1 closed=1 task=Pro_Audio timer=high_resolution cadence=rational_no_skips priority_before=0 priority_during=0 priority_after=0 failure=0 platform_error=0 cleanup_error=0\n'
                'SERVICE_CPU10_PEER pid=0 image=unknown site=none win32_error=0 exit_known=0 exit_code=0 exit_query_error=0 scans=301 maximum_scan_seconds=0.001\n'
                f'SERVICE_CPU10_RESULT row={row} phase=workload modes=128 power_ok=1 affinity_ok=1 limits_mode=observe_all_callbacks affinity_mode=single_fast_nonzero_current_thread copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor={h["factor"]} rate={h["rate"]} block={h["block"]} factor={h["factor"]} scenario={h["scenario"]} samples={raw["samples"]} warm_completed={h["warm_completed"]} interrupted=0 interrupt_reason=none allocations={h["callback_allocations"]} frees={h["callback_frees"]} {metrics} pass={int(raw["processing_pass"])}\n'
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

class Cpu29SourceBindingTests(unittest.TestCase):
    envelope=Cpu10ReaderTests.envelope
    current='complete-modal128-periodic-proaudio-processing29-exact-outer-normal-envelope-v1'
    historical='complete-modal128-periodic-proaudio-processing28b-aftermmcss-normal-envelope-v1'
    def test_explicit_conditions_preserve_required_row_failure(self):
        for condition in (self.historical,self.current):
            with self.subTest(condition=condition),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root,5,'overrun',condition=condition)
                result=reader.audit_evidence(root)
                self.assertEqual(result['condition'],condition)
                self.assertEqual(result['status'],'NOT_PASS')
                self.assertFalse(result['processing_pass']);self.assertFalse(result['qualified_processing_pass']);self.assertFalse(result['resume_eligible'])
    def test_current_condition_requires_current_qualification(self):
        for status,condition in [('PASS_CPU28B_TOOLING',self.current),('PASS_CPU29_TOOLING',self.historical)]:
            with self.subTest(status=status,condition=condition),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root,condition=self.current)
                path=root/'qualification.json';q=json.loads(path.read_text());q.update(status=status,condition=condition);data=json.dumps(q).encode();path.write_bytes(data)
                path=root/'manifest.json';m=json.loads(path.read_text());m['qualification_sha256']=hashlib.sha256(data).hexdigest().upper();path.write_text(json.dumps(m))
                with self.assertRaisesRegex(ValueError,'qualification mismatch'):reader.audit_evidence(root)
    def test_unknown_condition_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root,condition=self.current+'-unapproved')
            with self.assertRaisesRegex(ValueError,'declared condition'):reader.audit_evidence(root)
    def test_current_condition_must_capture_changed_runtime_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root,condition=self.current)
            path=root/'manifest.json';m=json.loads(path.read_text());m['sources']=[];path.write_text(json.dumps(m))
            with self.assertRaisesRegex(ValueError,'ModalOuter'):reader.audit_evidence(root)
    def test_changed_runtime_source_hash_cannot_be_substituted(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.envelope(root,condition=self.current)
            source=root/'source/src/dsp/space/runtime/ModalOuter.cpp';source.write_bytes(source.read_bytes()+b'changed after capture')
            with self.assertRaisesRegex(ValueError,'hash mismatch'):reader.audit_evidence(root)

class Cpu28EnvelopeTests(unittest.TestCase):
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
        selection='\n'.join(f'SERVICE_CPU28_SELECTION row={row} modes=128 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor={factor} limits_mode=observe_all_callbacks affinity_mode=single_fast_nonzero_current_thread rendered=1 allocations=0 frees=0 valid=1 actual_timing=0' for row,factor in [(4,1),(10,2)])
        self.assertEqual(reader.verify_selection(selection),[4,10])
        for old,new in [(' limits_mode=observe_all_callbacks',''),('limits_mode=observe_all_callbacks','limits_mode=stop_after_overrun'),('converter_factor=2','converter_factor=1'),('valid=1','valid=0'),('allocations=0','allocations=1'),('SERVICE_CPU28_SELECTION','SERVICE_CPU24_SELECTION')]:
            with self.subTest(mutant=new),self.assertRaises(ValueError):reader.verify_selection(selection.replace(old,new))
    @unittest.skipUnless(os.environ.get('AIRACK_CPU28_NATIVE_OBSERVATION'),'Native complete-overrun observation is explicit qualification only')
    def test_actual_native_complete_stress_observation(self):
        raw=reader.decode_timings(Path(os.environ['AIRACK_CPU28_NATIVE_OBSERVATION']).read_bytes())
        self.assertEqual(raw['row'],4)
        for key in ['schema_valid','complete','workload_complete','known_failure','observation_valid','timing_only_failure']:self.assertTrue(raw[key],key)
        self.assertEqual(raw['kernel_overruns'],1);self.assertFalse(raw['processing_pass']);self.assertEqual(raw['samples'],raw['header']['measure_expected'])
        matrix=self.matrix();matrix[4]=reader.classify_attempt(4,1,raw,phase='workload',footer_complete=True)
        report=reader.aggregate_attempts(matrix);self.assertTrue(report['qualified_processing_pass']);self.assertFalse(report['processing_pass']);self.assertEqual(report['observed_rows'],24)

class Cpu28AffinityTests(unittest.TestCase):
    envelope=Cpu10ReaderTests.envelope
    rebind_stdout=Cpu10ReaderTests.rebind_stdout
    def verify(self,text=None,phase='workload',reason='none'):
        return reader._verify_affinity(CPU_SETS+'\n'+AFFINITY if text is None else text,{'affinity_mode':'single_fast_nonzero_current_thread','affinity_ok':'0' if phase=='admission' else '1','interrupt_reason':reason},phase)
    def test_result_affinity_ok_is_required_and_exact(self):
        for value in [None,'0','2']:
            with self.subTest(value=value),self.assertRaises(ValueError):reader._verify_affinity(CPU_SETS+'\n'+AFFINITY,{'affinity_mode':'single_fast_nonzero_current_thread','affinity_ok':value},'workload')
    def test_exact_topology_selection_and_restore(self):
        result=self.verify();self.assertEqual(result['selected_index'],'1');self.assertEqual(len(result['cpu_sets']),4)
        self.assertEqual(self.verify('\n'.join(reversed(CPU_SETS.splitlines()))+'\n'+AFFINITY)['selected_id'],'11')
    def test_every_affinity_field_required_and_no_extra_or_duplicate(self):
        for token in AFFINITY.split()[1:]:
            with self.subTest(missing=token),self.assertRaises(ValueError):self.verify(CPU_SETS+'\n'+AFFINITY.replace(' '+token,''))
        for extra in [' attempted=1',' extra=1',' bare-token']:
            with self.subTest(extra=extra),self.assertRaises(ValueError):self.verify(CPU_SETS+'\n'+AFFINITY+extra)
    def test_affinity_and_restore_mutants(self):
        changes=[('attempted=1','attempted=0'),('opened=1','opened=0'),('restored=1','restored=0'),('close1=1','close1=0'),('close2=1','close2=0'),('setup_error=0','setup_error=5'),('cleanup_error=0','cleanup_error=5'),('topology_error=0','topology_error=5'),('group_count=1','group_count=2'),('before_ok=1','before_ok=0'),('during_ok=1','during_ok=0'),('after_ok=1','after_ok=0'),('before_group=0','before_group=1'),('during_group=0','during_group=1'),('after_group=0','after_group=1'),('after_mask=15','after_mask=14'),('during_mask=2','during_mask=0'),('during_mask=2','during_mask=6'),('selected_index=1','selected_index=0'),('selected_index=1','selected_index=64'),('selected_group=0','selected_group=1'),('selected_id=11','selected_id=12'),('selected_core=1','selected_core=2'),('selected_efficiency=2','selected_efficiency=1'),('eligible_mask=14','eligible_mask=6'),('performance_mask=6','performance_mask=2'),('before_mask=15','before_mask=13'),('after_mask=15','after_mask=18446744073709551616'),('during_mask=2','during_mask=+2')]
        for old,new in changes:
            with self.subTest(mutant=new),self.assertRaises(ValueError):self.verify(CPU_SETS+'\n'+AFFINITY.replace(old,new))
    def test_topology_derives_highest_eligible_and_lowest_nonzero(self):
        for old,new in [('id=12 group=0 index=2 core=2 efficiency=2','id=12 group=0 index=2 core=2 efficiency=3'),('id=11 group=0 index=1 core=1 efficiency=2 flags=0','id=11 group=0 index=1 core=1 efficiency=2 flags=1'),('id=11 group=0 index=1 core=1 efficiency=2 flags=0','id=11 group=0 index=1 core=1 efficiency=2 flags=2')]:
            with self.subTest(mutant=new),self.assertRaises(ValueError):self.verify(CPU_SETS.replace(old,new)+'\n'+AFFINITY)
        # Owned allocation is eligible, while index zero's higher class stays excluded.
        owned=CPU_SETS.replace('id=11 group=0 index=1 core=1 efficiency=2 flags=0','id=11 group=0 index=1 core=1 efficiency=2 flags=6')+'\n'+AFFINITY.replace('selected_flags=0','selected_flags=6')
        self.assertEqual(self.verify(owned)['selected_flags'],'6')
        later=AFFINITY.replace('during_mask=2','during_mask=4').replace('selected_id=11','selected_id=12').replace('selected_index=1','selected_index=2').replace('selected_core=1','selected_core=2').replace('selected_flags=0','selected_flags=6')
        with self.assertRaises(ValueError):self.verify(CPU_SETS+'\n'+later)
    def test_topology_identity_and_completeness(self):
        for table in [CPU_SETS+'\n'+CPU_SETS.splitlines()[0],CPU_SETS.replace('id=12','id=11'),CPU_SETS.replace('index=2','index=1'),'\n'.join(CPU_SETS.splitlines()[:-1]),'',CPU_SETS.replace('group=0','group=1'),CPU_SETS.replace('index=3','index=64'),CPU_SETS.replace('efficiency=1','efficiency=256'),CPU_SETS.replace('flags=6','flags=-1')]:
            with self.subTest(table=table),self.assertRaises(ValueError):self.verify(table+'\n'+AFFINITY)
    def test_exact_unsigned_mask_boundary_and_inherited_restriction(self):
        mask=1<<63
        table=f'SERVICE_CPU28_CPU id=10 group=0 index=0 core=0 efficiency=255 flags=0\nSERVICE_CPU28_CPU id=73 group=0 index=63 core=31 efficiency=2 flags=0'
        line=AFFINITY.replace('before_mask=15',f'before_mask={mask+1}').replace('during_mask=2',f'during_mask={mask}').replace('after_mask=15',f'after_mask={mask+1}').replace('selected_id=11','selected_id=73').replace('selected_index=1','selected_index=63').replace('selected_core=1','selected_core=31').replace('eligible_mask=14',f'eligible_mask={mask}').replace('performance_mask=6',f'performance_mask={mask}')
        self.assertEqual(self.verify(table+'\n'+line)['during_mask'],str(mask))
        restricted=AFFINITY.replace('before_mask=15','before_mask=9').replace('after_mask=15','after_mask=9').replace('during_mask=2','during_mask=8').replace('selected_id=11','selected_id=13').replace('selected_index=1','selected_index=3').replace('selected_core=1','selected_core=3').replace('selected_efficiency=2','selected_efficiency=1').replace('eligible_mask=14','eligible_mask=8').replace('performance_mask=6','performance_mask=8')
        self.assertEqual(self.verify(CPU_SETS+'\n'+restricted)['selected_index'],'3')
    def test_stress_hash_rebinding_cannot_hide_affinity_failure(self):
        mutations=[(b'after_mask=15',b'after_mask=14'),(b'during_mask=2',b'during_mask=6'),(b' selected_index=1',b' selected_index=0'),(b' selected_id=11',b' selected_id=12'),(b'affinity_mode=single_fast_nonzero_current_thread',b'affinity_mode=ordinary'),(b' affinity_mode=single_fast_nonzero_current_thread',b''),(b'affinity_ok=1',b'affinity_ok=0'),(b'affinity_ok=1',b'affinity_ok=2'),(b' affinity_ok=1',b''),(b'cleanup_error=0 topology_error=0',b'cleanup_error=5 topology_error=0'),(b'core=2 efficiency=2 flags=6',b'core=2 efficiency=3 flags=6')]
        for old,new in mutations:
            with self.subTest(mutant=new),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root,4,'overrun');data=(root/'row-04-attempt-1.stdout').read_bytes();self.assertIn(old,data);self.rebind_stdout(root,data.replace(old,new),4)
                with self.assertRaises(ValueError):reader.audit_evidence(root)
    def test_manifest_and_selection_must_declare_pinned_caller(self):
        for value in [None,'ordinary','process_affinity']:
            with self.subTest(mode=value),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);self.envelope(root);path=root/'manifest.json';manifest=json.loads(path.read_text());manifest['affinity_mode']=value;path.write_text(json.dumps(manifest))
                with self.assertRaises(ValueError):reader.audit_evidence(root)
        selection='\n'.join(f'SERVICE_CPU28_SELECTION row={row} modes=128 copy_mode=prepared scatter_mode=blocked reduction_mode=paired_stereo converter_mode=mirrored_avx2 converter_slots=8 converter_factor={factor} limits_mode=observe_all_callbacks affinity_mode=single_fast_nonzero_current_thread rendered=1 allocations=0 frees=0 valid=1 actual_timing=0' for row,factor in [(4,1),(10,2)])
        self.assertEqual(reader.verify_selection(selection),[4,10])
        for suffix in [' affinity_mode=ordinary','']:
            with self.subTest(suffix=suffix),self.assertRaises(ValueError):reader.verify_selection(selection.replace(' affinity_mode=single_fast_nonzero_current_thread',suffix))
    def test_untouched_only_before_admission_mutation(self):
        untouched='SERVICE_CPU28_AFFINITY '+' '.join(f'{token.split("=")[0]}={1 if token.split("=")[0] in ("close1","close2") else 0}' for token in AFFINITY.split()[1:])
        self.assertEqual(self.verify(untouched,'admission','peer_native_work')['attempted'],'0')
        for phase,reason in [('workload','peer_native_work'),('admission','none')]:
            with self.subTest(phase=phase,reason=reason),self.assertRaises(ValueError):self.verify(untouched,phase,reason)
        with self.assertRaises(ValueError):self.verify(CPU_SETS+'\n'+untouched,'admission','peer_native_work')
    @unittest.skipUnless(os.environ.get('AIRACK_CPU28_NATIVE_AFFINITY'),'Actual native affinity restoration is explicit qualification only')
    def test_actual_native_affinity_restoration(self):
        report=self.verify(Path(os.environ['AIRACK_CPU28_NATIVE_AFFINITY']).read_text(encoding='utf-8-sig'))
        self.assertGreater(int(report['selected_index']),0);self.assertEqual(report['before_mask'],report['after_mask']);self.assertGreater(len(report['cpu_sets']),1)

if __name__=='__main__':unittest.main()
