"""Independent ordered CPU10 evidence audit. This module never runs native code."""
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
CONDITION='complete-modal128-periodic-proaudio-processing24b-avx4-converter-ci-launcher-v2'
BINARY_NAME='RackSynthSpaceReverbModalAvx4ServiceProfileProbe.exe'
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
    known=bool(known or sum(durations)*2*rate>n*tps*block or q*2*rate>tps*block or copy_q*10*rate>tps*block or cold_q*2*rate>tps*block or overruns or h['callback_allocations'] or h['callback_frees'] or not 0<h['product_bytes']<=8*16*1024*1024 or complete and not workload)
    return {'schema_valid':True,'row':row,'header':h,'samples':n,'complete':complete,'known_failure':known,'processing_pass':complete and workload and not known,'workload_complete':workload,
            'mean_seconds':sum(durations)/(n*tps) if n else 0.,'p99_seconds':q/tps,'max_seconds':max(durations,default=0)/tps,'copy_p99_seconds':copy_q/tps,'cold_p99_seconds':cold_q/tps,
            'kernel_overruns':len(overruns),'dispatch_misses':len(dispatch),'room_copies':room,'post_admissions':post,'cold_installs':cold,'nine_room_callbacks':nine,
            'start_lateness_p99_seconds':_p99(lateness)/tps,'release_finish_p99_seconds':_p99(release_times)/tps,'kernel_overrun_records':overruns,'dispatch_miss_records':dispatch}

def classify_attempt(row,exit_code,raw,*,phase,reason='none',interrupted=False,caller_ok=True,footer_complete=False):
    _require(0<=row<24 and exit_code in (0,1,2,3),'attempt identity/exit')
    if raw is not None:_require(raw['row']==row,'raw attempt row')
    known=bool(exit_code==1 or raw is not None and raw['known_failure'])
    complete=bool(exit_code==0 and phase=='workload' and raw is not None and raw['processing_pass'] and not interrupted and caller_ok and footer_complete)
    eligible=bool(exit_code==3 and interrupted and caller_ok and not known and reason in PEER_REASONS and ((phase=='admission' and raw is None) or (phase=='workload' and raw is not None)))
    return {'row':row,'complete_pass':complete,'known_failure':known,'eligible_interruption':eligible,'valid_condition':complete or known or eligible,'exit':exit_code,'phase':phase,'reason':reason,'raw':raw}

def aggregate_attempts(attempts):
    completed={};bad=False
    for attempt in attempts:
        row=attempt['row'];_require(type(row) is int and 0<=row<24,'aggregate row')
        bad=bad or attempt['known_failure'] or not attempt['valid_condition']
        if attempt['complete_pass']:
            _require(row not in completed,'duplicate completed row');completed[row]=attempt
    passed=len(completed)==24 and not bad
    return {'status':'PASS_PROCESSING_CONDITIONAL' if passed else 'NOT_PASS' if bad else 'INCOMPLETE','processing_pass':passed,'resume_eligible':not bad and not passed,'completed_rows':len(completed),'accepted_rows':sorted(completed),
            'dispatch_misses_in_accepted_rows':sum((a.get('raw') or {}).get('dispatch_misses',0) for a in completed.values()),'attempts':attempts}


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

def evidence_inventory(root):
    return [{'file':p.name,'sha256':_sha(p)} for p in sorted(root.glob('row-*')) if p.is_file()]

def audit_evidence(root):
    root=Path(root).resolve();manifest_path=root/'manifest.json';m=_json(manifest_path)
    _require(m['condition']==CONDITION and m['rows']==24 and m['warm_seconds']==10 and m['measure_seconds']==20 and m['kernel_mean_p99_fraction']==.5 and m['copy_p99_fraction']==.1,'changed declared condition')
    _require(m.get('native_resource_lease')=='exclusive','exclusive native resource lease required')
    _bound(root,'RackSynthNativeRun.exe',m['native_launcher_sha256'])
    _bound(root,BINARY_NAME,m['binary_sha256']);_bound(root,'controller.ps1',m['controller_sha256']);_bound(root,'reader.py',m['reader_sha256'])
    _require(_sha(Path(__file__))==m['reader_sha256'].upper(),'running reader is not the frozen reader')
    q=_json(_bound(root,'qualification.json',m['qualification_sha256']));_require(q['status']=='PASS_CPU24B_TOOLING' and q['probe_binary_sha256'].upper()==m['binary_sha256'].upper() and q['controller_sha256'].upper()==m['controller_sha256'].upper() and q['reader_sha256'].upper()==m['reader_sha256'].upper(),'qualification mismatch')
    sources=set()
    for entry in m['sources']:
        _require(entry['path'] not in sources,'duplicate source');sources.add(entry['path']);_bound(root/'source',entry['path'],entry['sha256'])
    inventory=evidence_inventory(root);allowed=set();attempts=[];meta_by_id={}
    metadata=[]
    for p in root.glob('row-*-attempt-*.json'):
        match=re.fullmatch(r'row-(\d{2})-attempt-(\d+)\.json',p.name);_require(bool(match),'unknown attempt metadata name');metadata.append((int(match[1]),int(match[2]),p))
    for row,ordinal,path in sorted(metadata):
        _require(0<=row<24 and ordinal>=1,'attempt row/number');meta=_json(path);_require(meta['row']==row and meta['attempt']==ordinal and type(meta['exit']) is int,'attempt metadata');prefix=path.name[:-5];files={};allowed.add(path.name)
        for entry in meta['records']:
            name=entry['file'];_require(name.startswith(prefix+'.') and name[len(prefix):] in ('.stdout','.stderr','.timings.bin') and name not in files,'attempt record name')
            file=_bound(root,name,entry['sha256']);_require(file.stat().st_size==entry['bytes'],'record byte count');files[name[len(prefix):]]=file;allowed.add(name)
        _require('.stdout' in files and '.stderr' in files,'missing process output');text=files['.stdout'].read_text(encoding='utf-8-sig');result=_one(text,'SERVICE_CPU10_RESULT');footer=_one(text,'SERVICE_CPU10_FOOTER');caller=_one(text,'SERVICE_CPU10_CALLER');peer=_one(text,'SERVICE_CPU10_PEER')
        _require(int(result['row'])==row==int(footer['row']) and footer['acceptance']=='0','native footer identity')
        phase=result['phase'];_require(phase in ('workload','admission'),'unknown native phase')
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
        item=classify_attempt(row,meta['exit'],raw,phase=phase,reason=result['interrupt_reason'],interrupted=result['interrupted']=='1',caller_ok=caller_ok,footer_complete=footer['complete']=='1')
        _require((result['pass']=='1')==item['complete_pass'],'native pass/exit mismatch')
        item.update(attempt=ordinal,metadata_sha256=_sha(path),records=meta['records'],caller=caller,peer=peer);attempts.append(item);meta_by_id[row,ordinal]=item
    for p in root.glob('row-*.complete.json'):
        match=re.fullmatch(r'row-(\d{2})\.complete\.json',p.name);_require(bool(match),'unknown completed marker');meta=_json(p);row=int(match[1]);item=meta_by_id.get((row,meta['attempt']));_require(item is not None and meta['exit']==item['exit'],'completed marker identity')
        if meta['exit']==0:_require(meta['valid'] is True and item['complete_pass'],'false completed marker')
        allowed.add(p.name)
    _require({entry['file'] for entry in inventory}==allowed,'orphaned/unclassified native files')
    report=aggregate_attempts(attempts);report.update(condition=CONDITION,manifest_sha256=_sha(manifest_path),reader_sha256=_sha(Path(__file__)),inventory=inventory,qualification_sha256=m['qualification_sha256'],source_bindings=len(sources),scope='Conditional processing capacity only. Synthetic dispatch lateness remains separately reported; no real-host scheduling/installation/listening verdict.')
    return report

def _write_new(path,value):
    path=Path(path);_require(not path.exists(),'output exists: '+str(path));path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('evidence',nargs='?');parser.add_argument('--raw');parser.add_argument('--output');parser.add_argument('--resume-audit',action='store_true');args=parser.parse_args()
    try:
        if args.raw:
            report=decode_timings(Path(args.raw).read_bytes());code=0 if report['processing_pass'] else 1 if report['known_failure'] else 3
        else:
            _require(bool(args.evidence),'evidence root required');report=audit_evidence(args.evidence);code=0 if report['processing_pass'] else 1 if report['status']=='NOT_PASS' else 3
            if args.resume_audit:
                root=Path(args.evidence);index=1
                while (root/f'resume-audit-{index}.json').exists():index+=1
                token={'eligible':report['resume_eligible'],'manifest_sha256':report['manifest_sha256'],'reader_sha256':report['reader_sha256'],'accepted_rows':report['accepted_rows'],'inventory':report['inventory']}
                _write_new(root/f'resume-audit-{index}.json',token);report['resume_audit_file']=f'resume-audit-{index}.json'
        if args.output:_write_new(args.output,report)
        summary={key:report[key] for key in ['status','processing_pass','completed_rows','resume_eligible','dispatch_misses_in_accepted_rows','resume_audit_file'] if key in report};print(json.dumps(summary or {'processing_pass':report['processing_pass'],'known_failure':report['known_failure']}));return code
    except (ValueError,KeyError,OSError,TypeError,json.JSONDecodeError) as error:
        report={'status':'INVALID_EVIDENCE','processing_pass':False,'resume_eligible':False,'error':str(error)}
        if args.output and not Path(args.output).exists():_write_new(args.output,report)
        print(json.dumps(report));return 2

if __name__=='__main__':raise SystemExit(main())
