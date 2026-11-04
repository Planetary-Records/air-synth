"""Independent CPU28B through CPU33 full-observation audit with unchanged strict CPU10 budgets."""
import math
import struct
import argparse
import hashlib
import json
import re
from pathlib import Path

HEADER=struct.Struct('<Q6I12Q')
RECORD=struct.Struct('<14Q')
MAGIC=0x3155504356525353
HISTORICAL_CONDITION='complete-modal128-periodic-proaudio-processing28b-aftermmcss-normal-envelope-v1'
CONDITION='complete-modal128-periodic-proaudio-processing29-exact-outer-normal-envelope-v1'
RESERVED_CONDITION='complete-modal128-periodic-proaudio-processing30-reserved-caller-all-deadlines-v1'
TIMER_CONDITION='complete-modal128-periodic-proaudio-processing31-reserved-caller-honored-timers-all-deadlines-v1'
STABLE_CONDITION='complete-modal128-periodic-proaudio-processing32-stable-caller-honored-timers-all-deadlines-v1'
LOCAL_CONDITION='complete-modal128-periodic-proaudio-processing33-local-preemptible-helper-all-deadlines-v1'
STABLE_AFFINITY_MODE='single_fast_stable_reserved01_current_thread'
QUALIFICATION_STATUS={HISTORICAL_CONDITION:'PASS_CPU28B_TOOLING',CONDITION:'PASS_CPU29_TOOLING',RESERVED_CONDITION:'PASS_CPU30_TOOLING',TIMER_CONDITION:'PASS_CPU31_TOOLING',STABLE_CONDITION:'PASS_CPU32_TOOLING',LOCAL_CONDITION:'PASS_CPU33_TOOLING'}
BINARY_NAME='RackSynthSpaceReverbModalPinnedProfileProbe.exe'
SUPPORTED_ROWS=(0,1,2,3,5,12,13,14,15,17)
LIMITS_MODE='observe_all_callbacks'
AFFINITY_MODE='single_fast_nonzero_current_thread'
RESERVED_AFFINITY_MODE='single_fast_reserved01_current_thread'
HEADER_NAMES=('magic','version','row','rate','block','factor','scenario','warm_expected','measure_expected','ticks_per_second','measure_anchor','warm_start','warm_end','warm_completed','record_count','measure_end','callback_allocations','callback_frees','product_bytes')
PEER_REASONS={'peer_native_work','process_inventory_unavailable','inventory_monitor_stale'}

def _require(condition,message):
    if not condition:raise ValueError(message)

def _p99(values):
    return sorted(values)[(99*len(values)+99)//100-1] if values else 0

def decode_timings(data):
    _require(len(data)>=HEADER.size,'truncated header')
    h=dict(zip(HEADER_NAMES,HEADER.unpack_from(data)))
    _require(h['magic']==MAGIC and h['version']==1 and 0<=h['row']<24,'unknown header')
    row=h['row'];i=row%12;rate=[44100,48000,96000][i//2%3];block=[64,512][i%2];factor=i//6+1;scene=row//12;tps=h['ticks_per_second']
    _require((h['rate'],h['block'],h['factor'],h['scenario'])==(rate,block,factor,scene),'case mapping')
    _require(0<tps<=10**9,'clock frequency')
    warm=(10*rate+block-1)//block;expected=(20*rate+block-1)//block;n=h['record_count'];anchor=h['measure_anchor']
    _require(h['warm_expected']==warm and h['measure_expected']==expected and n<=expected,'callback counts')
    _require(len(data)==HEADER.size+n*RECORD.size,'raw length')
    _require(h['warm_completed']<=warm and h['warm_end']>=h['warm_start'] and h['measure_end']>=anchor,'header chronology')
    for key in ['measure_anchor','measure_end','warm_start','warm_end']:_require(h[key]<=2**63-1,'signed clock range')
    if n:_require(h['warm_completed']==warm and anchor>=h['warm_end'],'measured before warm completion')
    durations=[];copies=[];cold_times=[];lateness=[];release_times=[];overruns=[];dispatch=[];allocations=frees=room=post=cold=nine=0;previous=0;known=False
    cold_every=(rate+4*block-1)//(4*block)
    for ordinal in range(n):
        r=RECORD.unpack_from(data,HEADER.size+ordinal*RECORD.size)
        release,start,begin_finish,finish,cycles,room_copy,post_admission,cold_install,maximum_rooms,allocated,freed,peak_bits,checksum_bits,valid=r
        peak=struct.unpack('<d',struct.pack('<Q',peak_bits))[0];checksum=struct.unpack('<d',struct.pack('<Q',checksum_bits))[0]
        expected_release=anchor+(ordinal+1)*tps*block//rate;deadline=anchor+(ordinal+2)*tps*block//rate
        _require(release==expected_release and release<=start<=begin_finish<=finish<=h['measure_end'] and start>=previous and finish<=2**63-1,'record chronology')
        _require(room_copy<=1 and post_admission<=1 and cold_install<=1 and valid<=1 and room_copy+post_admission<=1,'record flags')
        _require(maximum_rooms in (8,9) and (not room_copy or maximum_rooms==9) and (not post_admission or maximum_rooms==8),'ticket/render count')
        _require(math.isfinite(peak) and peak>=0 and math.isfinite(checksum),'raw output nonfinite')
        if scene:_require(cold_install==int((warm+ordinal)%cold_every==0) and room_copy==post_admission==0 and maximum_rooms==8,'cold cadence/workload')
        else:_require(cold_install==0,'dynamic cold flag')
        duration=finish-start;durations.append(duration);lateness.append(start-release);release_times.append(finish-release);previous=finish
        room+=room_copy;post+=post_admission;cold+=cold_install;nine+=int(maximum_rooms==9);allocations+=allocated;frees+=freed
        if room_copy:copies.append(begin_finish-start)
        if cold_install:cold_times.append(duration)
        if not valid or allocated or freed:known=True
        event={'ordinal':ordinal,'kernel_ticks':duration,'cycles':cycles,'room_copy':room_copy,'cold_install':cold_install,'start_lateness_ticks':start-release,'release_finish_ticks':finish-release}
        if duration*rate>tps*block:overruns.append(event)
        if finish>deadline:dispatch.append(event)
    _require(allocations<=h['callback_allocations'] and frees<=h['callback_frees'],'allocation accounting')
    q=_p99(durations);copy_q=_p99(copies);cold_q=_p99(cold_times)
    complete=h['warm_completed']==warm and n==expected and h['warm_end']-h['warm_start']>=10*tps and h['measure_end']-anchor>=20*tps
    workload=cold>=16 if scene else room>=16 and post>=16 and nine*4>=n
    timing_failure=bool(sum(durations)*2*rate>n*tps*block or q*2*rate>tps*block or copy_q*10*rate>tps*block or cold_q*2*rate>tps*block or overruns)
    data_contract_valid=bool(not known and not h['callback_allocations'] and not h['callback_frees'] and 0<h['product_bytes']<=8*16*1024*1024)
    known=bool(not data_contract_valid or timing_failure or complete and not workload)
    observed=bool(complete and workload and data_contract_valid)
    return {'schema_valid':True,'row':row,'header':h,'samples':n,'complete':complete,'known_failure':known,'processing_pass':complete and workload and not known,'workload_complete':workload,
            'data_contract_valid':data_contract_valid,'timing_failure':timing_failure,'observation_valid':observed,'timing_only_failure':observed and timing_failure,
            'mean_seconds':sum(durations)/(n*tps) if n else 0.,'p99_seconds':q/tps,'max_seconds':max(durations,default=0)/tps,'copy_p99_seconds':copy_q/tps,'cold_p99_seconds':cold_q/tps,
            'kernel_overruns':len(overruns),'dispatch_misses':len(dispatch),'room_copies':room,'post_admissions':post,'cold_installs':cold,'nine_room_callbacks':nine,
            'start_lateness_p99_seconds':_p99(lateness)/tps,'release_finish_p99_seconds':_p99(release_times)/tps,'kernel_overrun_records':overruns,'dispatch_miss_records':dispatch}

def classify_attempt(row,exit_code,raw,*,phase,reason='none',interrupted=False,caller_ok=True,peer_ok=True,footer_complete=False):
    _require(0<=row<24 and exit_code in (0,1,2,3),'attempt identity/exit')
    if raw is not None:_require(raw['row']==row,'raw attempt row')
    known=bool(exit_code==1 or raw is not None and raw['known_failure'])
    observed=bool(phase=='workload' and raw is not None and raw['observation_valid'] and not interrupted and reason=='none' and caller_ok and peer_ok and footer_complete and exit_code==(0 if raw['processing_pass'] else 1))
    complete=bool(observed and raw['processing_pass'])
    qualified=bool(observed and (complete or row not in SUPPORTED_ROWS and raw['timing_only_failure']))
    return {'row':row,'complete_pass':complete,'complete_observation':observed,'qualified_row_pass':qualified,'known_failure':known,'eligible_interruption':False,'valid_condition':observed,'exit':exit_code,'phase':phase,'reason':reason,'raw':raw}

def aggregate_attempts(attempts):
    completed={};observed={};seen=set();bad=False
    for attempt in attempts:
        row=attempt['row'];_require(type(row) is int and 0<=row<24,'aggregate row')
        _require(row not in seen,'duplicate row; CPU28 does not replay/resume');seen.add(row)
        bad=bad or not attempt.get('qualified_row_pass',False)
        if attempt.get('complete_observation',False):observed[row]=attempt
        if attempt['complete_pass']:
            _require(row not in completed,'duplicate completed row');completed[row]=attempt
    passed=len(completed)==24 and not bad
    qualified=len(observed)==24 and not bad
    return {'status':'PASS_PROCESSING_CONDITIONAL' if passed else 'PASS_QUALIFIED_NORMAL_ENVELOPE' if qualified else 'NOT_PASS' if bad else 'INCOMPLETE','processing_pass':passed,'qualified_processing_pass':qualified,'resume_eligible':False,'completed_rows':len(completed),'accepted_rows':sorted(completed),'observed_rows':len(observed),'observed_row_ids':sorted(observed),'qualified_rows':list(SUPPORTED_ROWS),'qualified_rows_passed':[row for row in SUPPORTED_ROWS if row in completed],
            'dispatch_misses_in_accepted_rows':sum((a.get('raw') or {}).get('dispatch_misses',0) for a in completed.values()),'dispatch_misses_in_observed_rows':sum(a['raw']['dispatch_misses'] for a in observed.values()),'attempts':attempts}


def _sha(path):return hashlib.sha256(path.read_bytes()).hexdigest().upper()
def _json(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def _safe(root,name):
    relative=Path(name.replace('\\','/'));_require(not relative.is_absolute() and '..' not in relative.parts,'unsafe evidence path')
    path=(root/relative).resolve();_require(path.is_relative_to(root.resolve()),'evidence path escapes root');return path

def _bound(root,name,digest):
    path=_safe(root,name);_require(path.is_file() and isinstance(digest,str) and re.fullmatch('[0-9A-Fa-f]{64}',digest) and _sha(path)==digest.upper(),'hash mismatch: '+name);return path

def _one(text,prefix):
    lines=[line for line in text.splitlines() if line.startswith(prefix+' ')];_require(len(lines)==1,'missing/duplicate '+prefix)
    fields={}
    for token in lines[0].split()[1:]:
        _require(token.count('=')==1,'malformed '+prefix);key,value=token.split('=');_require(key not in fields,'duplicate field '+key);fields[key]=value
    return fields

def _verify_power(text,result,phase,mechanisms=1):
    _require(mechanisms in (1,5),'unknown process power policy')
    p=_one(text,'SERVICE_CPU16_POWER')
    expected={'attempted','opened','restored','close1','close2','setup_error','cleanup_error'}|{prefix+'_'+key for prefix in ('before','during','after') for key in ('version','control','state')}
    _require(set(p)==expected,'unexpected/missing power fields')
    for key in ('attempted','opened','restored','close1','close2'):_require(p.get(key) in ('0','1'),'invalid power flag '+key)
    _require(p.get('setup_error')=='0' and p.get('cleanup_error')=='0' and p['close1']==p['close2']=='1','power setup/cleanup failure')
    states=[]
    for prefix in ('before','during','after'):
        tokens=tuple(p[prefix+'_'+key] for key in ('version','control','state'))
        _require(all(re.fullmatch('[0-9]+',token) for token in tokens),'invalid power state number')
        value=tuple(map(int,tokens))
        _require(all(0<=n<=0xffffffff for n in value),'power state range');states.append(value)
    before,during,after=states
    if p['attempted']=='0':
        _require(phase=='admission' and result['interrupt_reason'] in PEER_REASONS and result.get('power_ok')=='0' and p['opened']==p['restored']=='0' and before==during==after==(0,0,0),'unverified power admission')
    else:
        _require(result.get('power_ok')=='1' and p['opened']==p['restored']=='1' and before[0]==1 and after==before and during==(before[0],before[1]|mechanisms,before[2]&~mechanisms),'power readback/restoration mismatch')
    return p

def _verify_affinity(text,result,phase,mode=None):
    p=_one(text,'SERVICE_CPU28_AFFINITY')
    flags={'attempted','opened','restored','close1','close2','before_ok','during_ok','after_ok'}
    errors={'setup_error','cleanup_error','topology_error'}
    masks={'before_mask','during_mask','after_mask','eligible_mask','performance_mask'}
    groups={'before_group','during_group','after_group','selected_group','group_count'}
    selected={'selected_id','selected_index','selected_core','selected_efficiency','selected_flags'}
    _require(set(p)==flags|errors|masks|groups|selected,'unexpected/missing affinity fields')
    mode=result.get('affinity_mode') if mode is None else mode
    _require(mode in (AFFINITY_MODE,RESERVED_AFFINITY_MODE,STABLE_AFFINITY_MODE) and result.get('affinity_mode')==mode,'native affinity mode')
    values={}
    for key,token in p.items():
        _require(re.fullmatch('[0-9]+',token) is not None,'invalid affinity number '+key)
        value=int(token);maximum=(1<<64)-1 if key in masks else (1<<16)-1 if key in groups else 255 if key in selected-{'selected_id'} else (1<<32)-1
        _require(0<=value<=maximum,'affinity number range '+key);values[key]=value
    _require(all(values[key] in (0,1) for key in flags),'invalid affinity flags')
    _require(values['close1']==values['close2']==1 and all(values[key]==0 for key in errors),'affinity setup/cleanup failure')
    lines=[line for line in text.splitlines() if line.startswith('SERVICE_CPU28_CPU ')]
    if values['attempted']==0:
        _require(phase=='admission' and result.get('affinity_ok')=='0' and result.get('interrupt_reason') in PEER_REASONS and not lines and all(value==(1 if key in ('close1','close2') else 0) for key,value in values.items()),'unverified affinity admission')
        return {**p,'cpu_sets':[]}
    _require(result.get('affinity_ok')=='1','native affinity qualification flag')
    _require(all(values[key]==1 for key in flags) and values['group_count']==1,'affinity placement/restoration flags')
    _require(1<=len(lines)<=64,'affinity CPU-set count')
    cpus=[];ids=set();indices=set();topology_mask=0
    for line in lines:
        fields=_one(line,'SERVICE_CPU28_CPU')
        _require(set(fields)=={'id','group','index','core','efficiency','flags'},'CPU-set fields')
        cpu={}
        for key,token in fields.items():
            _require(re.fullmatch('[0-9]+',token) is not None,'invalid CPU-set number')
            value=int(token);maximum=(1<<32)-1 if key=='id' else (1<<16)-1 if key=='group' else 255
            _require(0<=value<=maximum,'CPU-set range');cpu[key]=value
        _require(cpu['group']==0 and cpu['index']<64 and cpu['id'] not in ids and cpu['index'] not in indices,'CPU-set identity/group')
        ids.add(cpu['id']);indices.add(cpu['index']);topology_mask|=1<<cpu['index'];cpus.append(cpu)
    before=(values['before_group'],values['before_mask']);during=(values['during_group'],values['during_mask']);after=(values['after_group'],values['after_mask'])
    _require(before[0]==during[0]==after[0]==values['selected_group']==0 and before[1]!=0 and after==before,'affinity group or exact restoration')
    _require(before[1]&~topology_mask==0,'affinity topology omits inherited CPUs')
    eligible=[cpu for cpu in cpus if cpu['index']>0 and before[1]&(1<<cpu['index']) and (mode==STABLE_AFFINITY_MODE or not cpu['flags']&1) and not (cpu['flags']&2 and not cpu['flags']&4)]
    if mode in (RESERVED_AFFINITY_MODE,STABLE_AFFINITY_MODE):
        reserved={cpu['core'] for cpu in cpus if cpu['index']<2}
        eligible=[cpu for cpu in eligible if cpu['core'] not in reserved]
    _require(bool(eligible),'no eligible nonzero caller CPU')
    highest=max(cpu['efficiency'] for cpu in eligible);performance=[cpu for cpu in eligible if cpu['efficiency']==highest]
    chosen=(max if mode==STABLE_AFFINITY_MODE else min)(performance,key=lambda cpu:cpu['index'])
    _require(all(values['selected_'+key]==chosen[key] for key in chosen),'affinity selected CPU differs from derived topology')
    wanted=1<<chosen['index'];eligible_mask=sum(1<<cpu['index'] for cpu in eligible);performance_mask=sum(1<<cpu['index'] for cpu in performance)
    _require(values['eligible_mask']==eligible_mask and values['performance_mask']==performance_mask,'affinity eligibility/performance masks')
    _require(during[1]==wanted and wanted&before[1]==wanted and wanted.bit_count()==1,'affinity one-bit subset placement')
    return {**p,'cpu_sets':cpus}

def verify_selection(text,mode=AFFINITY_MODE):
    lines=[line for line in text.splitlines() if line.startswith('SERVICE_CPU28_SELECTION ')]
    _require(len(lines)==2,'selection row count')
    rows=[]
    for line in lines:
        selected=_one(line,'SERVICE_CPU28_SELECTION');row=int(selected['row'])
        _require(row in (4,10) and row not in rows,'selection row identity');rows.append(row)
        wanted={'modes':'128','copy_mode':'prepared','scatter_mode':'blocked','reduction_mode':'paired_stereo','converter_mode':'mirrored_avx2','converter_slots':'8','converter_factor':str((row%12)//6+1),'limits_mode':LIMITS_MODE,'affinity_mode':mode,'rendered':'1','allocations':'0','frees':'0','valid':'1','actual_timing':'0'}
        _require(all(selected.get(key)==value for key,value in wanted.items()),'selection condition mismatch')
    return rows

def _peer_ok(peer):
    expected={'pid':'0','image':'unknown','site':'none','win32_error':'0','exit_known':'0','exit_code':'0','exit_query_error':'0'}
    _require(set(peer)==set(expected)|{'scans','maximum_scan_seconds'},'peer fields')
    _require(re.fullmatch('[0-9]+',peer['scans']) is not None,'peer scan count')
    duration=float(peer['maximum_scan_seconds'])
    _require(math.isfinite(duration) and duration>=0,'peer scan duration')
    return all(peer[key]==value for key,value in expected.items()) and int(peer['scans'])>0

def evidence_inventory(root):
    return [{'file':p.name,'sha256':_sha(p)} for p in sorted(root.glob('row-*')) if p.is_file()]

def audit_evidence(root):
    root=Path(root).resolve();manifest_path=root/'manifest.json';m=_json(manifest_path)
    mode=STABLE_AFFINITY_MODE if m['condition'] in (STABLE_CONDITION,LOCAL_CONDITION) else RESERVED_AFFINITY_MODE if m['condition'] in (RESERVED_CONDITION,TIMER_CONDITION) else AFFINITY_MODE
    _require(m['condition'] in QUALIFICATION_STATUS and m['rows']==24 and m['warm_seconds']==10 and m['measure_seconds']==20 and m['kernel_mean_p99_fraction']==.5 and m['copy_p99_fraction']==.1,'changed declared condition')
    _require(m.get('limits_mode')==LIMITS_MODE and m.get('supported_rows')==list(SUPPORTED_ROWS) and m.get('affinity_mode')==mode,'changed qualified envelope')
    _require(m.get('native_resource_lease')=='exclusive','exclusive native resource lease required')
    _bound(root,'RackSynthNativeRun.exe',m['native_launcher_sha256'])
    _bound(root,BINARY_NAME,m['binary_sha256']);_bound(root,'controller.ps1',m['controller_sha256']);_bound(root,'reader.py',m['reader_sha256'])
    _require(_sha(Path(__file__))==m['reader_sha256'].upper(),'running reader is not the frozen reader')
    q=_json(_bound(root,'qualification.json',m['qualification_sha256']));_require(q['status']==QUALIFICATION_STATUS[m['condition']] and (m['condition']==HISTORICAL_CONDITION or q.get('condition')==m['condition']) and q['probe_binary_sha256'].upper()==m['binary_sha256'].upper() and q['controller_sha256'].upper()==m['controller_sha256'].upper() and q['reader_sha256'].upper()==m['reader_sha256'].upper(),'qualification mismatch')
    sources=set()
    for entry in m['sources']:
        _require(entry['path'] not in sources,'duplicate source');sources.add(entry['path']);_bound(root/'source',entry['path'],entry['sha256'])
    if m['condition'] in (CONDITION,RESERVED_CONDITION,TIMER_CONDITION,STABLE_CONDITION,LOCAL_CONDITION):
        _require('src/dsp/space/runtime/ModalOuter.cpp' in {path.replace('\\','/') for path in sources},'CPU29 must capture changed ModalOuter source')
    inventory=evidence_inventory(root);allowed=set();attempts=[];meta_by_id={}
    metadata=[]
    for p in root.glob('row-*-attempt-*.json'):
        match=re.fullmatch(r'row-(\d{2})-attempt-(\d+)\.json',p.name);_require(bool(match),'unknown attempt metadata name');metadata.append((int(match[1]),int(match[2]),p))
    for row,ordinal,path in sorted(metadata):
        _require(0<=row<24 and ordinal==1,'attempt row/number; CPU28 does not replay/resume');meta=_json(path);_require(meta['row']==row and meta['attempt']==ordinal and type(meta['exit']) is int,'attempt metadata');prefix=path.name[:-5];files={};allowed.add(path.name)
        for entry in meta['records']:
            name=entry['file'];_require(name.startswith(prefix+'.') and name[len(prefix):] in ('.stdout','.stderr','.timings.bin') and name not in files,'attempt record name')
            file=_bound(root,name,entry['sha256']);_require(file.stat().st_size==entry['bytes'],'record byte count');files[name[len(prefix):]]=file;allowed.add(name)
        _require('.stdout' in files and '.stderr' in files,'missing process output');text=files['.stdout'].read_text(encoding='utf-8-sig');result=_one(text,'SERVICE_CPU10_RESULT');footer=_one(text,'SERVICE_CPU10_FOOTER');caller=_one(text,'SERVICE_CPU10_CALLER');peer=_one(text,'SERVICE_CPU10_PEER')
        _require(int(result['row'])==row==int(footer['row']) and footer['acceptance']=='0','native footer identity')
        phase=result['phase'];_require(phase in ('workload','admission'),'unknown native phase')
        _require(result.get('limits_mode')==LIMITS_MODE,'native callback observation mode')
        power=_verify_power(text,result,phase,5 if m['condition'] in (TIMER_CONDITION,STABLE_CONDITION,LOCAL_CONDITION) else 1)
        affinity=_verify_affinity(text,result,phase,mode)
        _require(result.get('modes')=='128' if phase=='workload' else result.get('modes') in ('0','128'),'native density mismatch')
        _require(result.get('copy_mode')=='prepared' if phase=='workload' else (result.get('modes'),result.get('copy_mode')) in (('0','none'),('128','prepared')),'native copy mode mismatch')
        _require(result.get('scatter_mode')=='blocked' if phase=='workload' else (result.get('modes'),result.get('scatter_mode')) in (('0','none'),('128','blocked')),'native scatter mode mismatch')
        _require(result.get('reduction_mode')=='paired_stereo' if phase=='workload' else (result.get('modes'),result.get('reduction_mode')) in (('0','none'),('128','paired_stereo')),'native reduction mode mismatch')
        expected_converter=('mirrored_avx2','8',str((row%12)//6+1))
        converter=tuple(result.get(k) for k in ('converter_mode','converter_slots','converter_factor'))
        _require(converter==expected_converter if phase=='workload' else (result.get('modes'),converter) in (('0',('none','0','0')),('128',expected_converter)),'native converter selection mismatch')
        raw=decode_timings(files['.timings.bin'].read_bytes()) if '.timings.bin' in files else None
        _require((raw is not None)==(phase=='workload'),'phase/raw mismatch')
        _require(caller['task']=='Pro_Audio' and caller['timer']=='high_resolution' and caller['cadence']=='rational_no_skips','caller condition')
        if m['condition'] in (STABLE_CONDITION,LOCAL_CONDITION):_require(caller.get('helper_minimum_sleep_100ns')=='10000','helper minimum sleep policy')
        if m['condition']==LOCAL_CONDITION:_require(caller.get('helper_policy')=='local_preemptible_non_mmcss15' and caller.get('caller_precise_wait_ns')=='150000','local preemptible helper and caller guard')
        priorities=[int(caller[k]) for k in ('priority_before','priority_during','priority_after')]
        caller_ok=caller['closed']=='1' and caller['cleanup_error']=='0' and caller['failure']=='0' and caller['platform_error']=='0' and priorities[0]==priorities[2] and 2147483647 not in priorities
        if phase=='workload':caller_ok=caller_ok and caller['opened']=='1' and caller['allocation_hook_calibrated']=='1'
        _require(result['interrupted'] in ('0','1') and result['pass'] in ('0','1') and footer['complete'] in ('0','1'),'native disposition flags')
        if raw is not None:
            _require(raw['row']==row and int(result['samples'])==raw['samples'],'native sample count')
            h=raw['header']
            for key in ['rate','block','factor','scenario']:_require(int(result[key])==h[key],'native case '+key)
            _require(int(result['warm_completed'])==h['warm_completed'] and int(result['allocations'])==h['callback_allocations'] and int(result['frees'])==h['callback_frees'],'native warm/allocation metadata')
            for key in ['schema_valid','complete','processing_pass','workload_complete','kernel_overruns','dispatch_misses','room_copies','post_admissions','cold_installs','nine_room_callbacks']:_require(int(result[key])==int(raw[key]),'native aggregate '+key)
            _require(int(result['known_failure'])>=int(raw['known_failure']),'native suppressed failed metric')
            if int(result['known_failure'])!=int(raw['known_failure']):_require(result['interrupt_reason']=='warm_callback_contract','unexplained native failure')
            for key in ['mean_seconds','p99_seconds','max_seconds','copy_p99_seconds','cold_p99_seconds','start_lateness_p99_seconds','release_finish_p99_seconds']:_require(math.isfinite(float(result[key])) and abs(float(result[key])-raw[key])<=1e-12*max(1,abs(raw[key])),'native numeric '+key)
            for prefix_name,key in [('SERVICE_CPU10_KERNEL_OVERRUN','kernel_overrun_records'),('SERVICE_CPU10_DISPATCH_MISS','dispatch_miss_records')]:
                actual=[int(dict(token.split('=') for token in line.split()[1:])['ordinal']) for line in text.splitlines() if line.startswith(prefix_name+' ')]
                _require(actual==[entry['ordinal'] for entry in raw[key]],'native missed ordinal list')
        else:
            _require(int(result['samples'])==0 and int(result['warm_completed'])==0 and result['pass']=='0' and footer['complete']=='0','nonempty admission phase')
            _require(not any(line.startswith('SERVICE_CPU10_BEGIN ') for line in text.splitlines()),'admission started timing')
        item=classify_attempt(row,meta['exit'],raw,phase=phase,reason=result['interrupt_reason'],interrupted=result['interrupted']=='1',caller_ok=caller_ok,peer_ok=_peer_ok(peer),footer_complete=footer['complete']=='1')
        _require((result['pass']=='1')==item['complete_pass'],'native pass/exit mismatch')
        item.update(attempt=ordinal,metadata_sha256=_sha(path),records=meta['records'],caller=caller,peer=peer,power=power,affinity=affinity);attempts.append(item);meta_by_id[row,ordinal]=item
    for p in root.glob('row-*.complete.json'):
        match=re.fullmatch(r'row-(\d{2})\.complete\.json',p.name);_require(bool(match),'unknown completed marker');meta=_json(p);row=int(match[1]);item=meta_by_id.get((row,meta['attempt']));_require(item is not None and meta['exit']==item['exit'],'completed marker identity')
        _require(type(meta['valid']) is bool and meta['valid']==item['valid_condition'],'false completed marker')
        expected_kind='passed' if item['complete_pass'] else 'stress_timing_observed' if item['qualified_row_pass'] else 'failed'
        _require(meta.get('kind')==expected_kind,'completed marker classification')
        allowed.add(p.name)
    _require({entry['file'] for entry in inventory}==allowed,'orphaned/unclassified native files')
    report=aggregate_attempts(attempts);report.update(condition=m['condition'],limits_mode=LIMITS_MODE,affinity_mode=mode,manifest_sha256=_sha(manifest_path),reader_sha256=_sha(Path(__file__)),inventory=inventory,qualification_sha256=m['qualification_sha256'],source_bindings=len(sources),scope='All 24 rows must be observed completely under the explicit HighQoS caller pinned only on its own thread to the lowest eligible CPU index in the highest eligible EfficiencyClass within its inherited single-group mask under the declared placement policy (CPU30-33 reserve physical cores containing logical processors 0 and 1; CPU31-33 additionally require explicit honoring of owned timer-resolution requests; CPU32/33 select the highest-index eligible fastest CPU independent of parking and require a minimum1ms helper sleep; CPU33 uses a local conventional-priority15 helper outside MMCSS so its spin cannot consume the caller scheduling category allowance), with exact affinity restoration. Every raw data, heap, storage, caller, power, topology, placement and peer contract must be valid. Qualified processing covers eight serial units in Normal at 44.1/48 kHz with 64/512 frames and at 96 kHz with 512 frames, live and cold. Normal 96 kHz/64 and all Higher rows are stress/offline observations. Original processing_pass still requires every original timing budget in all 24 rows. Dispatch scheduling, installation and human visual/listening acceptance remain separate.')
    return report

def require_all_deadlines(report):
    passed=bool(report['processing_pass'] and report['completed_rows']==24 and report['dispatch_misses_in_observed_rows']==0)
    report.update(all_deadlines_pass=passed,status='PASS_ALL24_DEADLINES' if passed else 'NOT_PASS_ALL24_DEADLINES',scope='All 24 original rows must pass every processing, heap, storage, caller, power, topology and peer contract with zero processing overruns AND release-relative completion misses. No stress/offline exemption or changed budget.')
    return passed

def _write_new(path,value):
    path=Path(path);_require(not path.exists(),'output exists: '+str(path));path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('evidence',nargs='?');parser.add_argument('--raw');parser.add_argument('--output');parser.add_argument('--require-all-deadlines',action='store_true');args=parser.parse_args()
    try:
        if args.raw:
            _require(not args.require_all_deadlines,'all-deadline acceptance requires a complete evidence packet')
            report=decode_timings(Path(args.raw).read_bytes());code=0 if report['processing_pass'] else 1 if report['known_failure'] else 3
        else:
            _require(bool(args.evidence),'evidence root required');report=audit_evidence(args.evidence);code=0 if report['qualified_processing_pass'] else 1 if report['status']=='NOT_PASS' else 3
            if args.require_all_deadlines:code=0 if require_all_deadlines(report) else 1
        if args.output:_write_new(args.output,report)
        summary={key:report[key] for key in ['status','processing_pass','qualified_processing_pass','all_deadlines_pass','observed_rows','completed_rows','resume_eligible','dispatch_misses_in_accepted_rows','dispatch_misses_in_observed_rows'] if key in report};print(json.dumps(summary or {'processing_pass':report['processing_pass'],'known_failure':report['known_failure']}));return code
    except (ValueError,KeyError,OSError,TypeError,json.JSONDecodeError) as error:
        report={'status':'INVALID_EVIDENCE','processing_pass':False,'qualified_processing_pass':False,'observed_rows':0,'resume_eligible':False,'error':str(error)}
        if args.output and not Path(args.output).exists():_write_new(args.output,report)
        print(json.dumps(report));return 2

if __name__=='__main__':raise SystemExit(main())
