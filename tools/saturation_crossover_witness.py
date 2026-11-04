"""Pure exact cell diagnostic. Never launches native code, timing, or audio generation."""
import argparse
import csv
from fractions import Fraction
import json
import math
import pathlib
import re
import struct
import sys
import datetime
import tempfile
import xml.etree.ElementTree as ET

import saturation_history_trace as trace
import saturation_numerical_stability as stability

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = 'frame,network,oversample,cell,network_id,cell_id,b0,b1,b2,a1,a2,x_l,x_r,z1_l,z1_r,z2_l,z2_r,y_l,y_r,next_z1_l,next_z1_r,next_z2_l,next_z2_r'.split(',')
ORDER = ['lowLP1','lowHP1','lowLP2','lowHP2','highLP1','highHP1','highLP2','highHP2','highAP']
FAMILIES = ['lowLP','lowHP','lowLP','lowHP','highLP','highHP','highLP','highHP','highAP']
PREDECLARATION = 'docs/verification/saturation-numerical-stability-20261022/task3-predeclaration.json'
LEDGER = 'docs/verification/saturation-decay-bounds-20261022/task1-decay-ledger.json'
CANDIDATE_SHA = '134416edd37ce9a4dbb4e20063b2d7fa0e98dade5f297ee0685a8ffce83f6e30'
require = stability.require
identity = stability.identity


def load(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON key: ' + key)
            result[key] = value
        return result
    def nonfinite(value):
        raise ValueError('nonfinite JSON constant: ' + value)
    try:
        return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique, parse_constant=nonfinite)
    except RecursionError as error:
        raise ValueError('JSON nesting exceeds decoder limit') from error


def bits(x):
    return struct.pack('>d', x)


def binary64(text):
    require(isinstance(text, str) and re.fullmatch(r'-?0x[0-9a-f]+(?:\.[0-9a-f]*)?p[+-][0-9]+', text) is not None,
            'finite hexadecimal binary64 field required')
    value = float.fromhex(text)
    require(math.isfinite(value), 'finite hexadecimal binary64 field required')
    return value


def replay(row, channel):
    """Nine separate binary64 operations, then exact rational ideal update."""
    b0,b1,b2,a1,a2 = [row[k] for k in ('b0','b1','b2','a1','a2')]
    x,z1,z2 = [row[k + '_' + channel] for k in ('x','z1','z2')]
    operations = []
    def operation(name, left, right, subtract=False, multiply=False):
        exact = Fraction(left) * Fraction(right) if multiply else Fraction(left) - Fraction(right) if subtract else Fraction(left) + Fraction(right)
        actual = left * right if multiply else left - right if subtract else left + right
        require(math.isfinite(actual), 'native replay nonfinite intermediate')
        operations.append(dict(operation=name, left=left.hex(), right=right.hex(), actual=actual.hex(), residual=Fraction(actual)-exact))
        return actual
    y = operation('y.add', operation('b0*x',b0,x,multiply=True),z1)
    n1 = operation('next1.add', operation('next1.subtract',operation('b1*x',b1,x,multiply=True),operation('a1*y',a1,y,multiply=True),subtract=True),z2)
    n2 = operation('next2.subtract',operation('b2*x',b2,x,multiply=True),operation('a2*y',a2,y,multiply=True),subtract=True)
    for name, expected in (('y',y),('next_z1',n1),('next_z2',n2)):
        require(bits(row[name+'_'+channel]) == bits(expected), 'native replay bit mismatch: ' + name + '_' + channel)
    b0,b1,b2,a1,a2,x,z1,z2 = map(Fraction,(b0,b1,b2,a1,a2,x,z1,z2))
    ideal_y = b0*x+z1
    ideal = [b1*x-a1*ideal_y+z2, b2*x-a2*ideal_y]
    return dict(output_residual=Fraction(y)-ideal_y, eta=[Fraction(n1)-ideal[0],Fraction(n2)-ideal[1]], operations=operations)


def paired_identity(left, right, channel):
    require(all(bits(left[k])==bits(right[k]) for k in ('b0','b1','b2','a1','a2')), 'paired exact coefficient identity')
    a,b = replay(left,channel),replay(right,channel)
    b0,b1,b2,a1,a2 = [Fraction(left[k]) for k in ('b0','b1','b2','a1','a2')]
    A = [[-a1,Fraction(1)],[-a2,Fraction(0)]]
    B = [b1-a1*b0,b2-a2*b0]
    delta = lambda name: Fraction(left[name+'_'+channel])-Fraction(right[name+'_'+channel])
    before,after = [delta(k) for k in ('z1','z2')],[delta(k) for k in ('next_z1','next_z2')]
    forcing = [a['eta'][k]-b['eta'][k] for k in range(2)]
    lhs = [after[k]-sum(A[k][j]*before[j] for j in range(2))-B[k]*delta('x') for k in range(2)]
    residual = [lhs[k]-forcing[k] for k in range(2)]
    require(residual == [0,0], 'exact paired state identity')
    return dict(A=A,B=B,delta_input=delta('x'),delta_before=before,delta_after=after,
                forcing=forcing,residual=residual,output_residual_difference=a['output_residual']-b['output_residual'],left=a,right=b)


def read_cells(meta_path, csv_path, expected, coefficients):
    meta = load(meta_path)
    require(isinstance(meta,dict) and meta == expected and all(type(meta[k]) is int for k in ('factor','rate','history','frame','source_mode','endpoints','records')),
            'cell metadata identity/mode/extent')
    require(meta['schema']=='saturation-crossover-cells-v1' and meta['status']=='complete' and meta['factor'] in (4,64)
            and meta['rate'] in (44100,48000,96000) and meta['history'] in (0,1,2) and meta['source_mode'] in (0,1)
            and meta['frame']==4096+2*meta['rate']-64 and meta['endpoints']==1 and meta['cell_order']==ORDER
            and meta['records']==(1152 if meta['factor']==64 and meta['source_mode']==1 else 0), 'cell metadata schema/extent')
    with csv_path.open(newline='',encoding='utf-8') as stream:
        reader=csv.DictReader(stream)
        require(reader.fieldnames==HEADER,'exact cell CSV header')
        raw=list(reader)
    require(len(raw)==meta['records'],'cell record extent')
    rows=[]; network_ids={}; cell_ids={}
    for index,item in enumerate(raw):
        require(set(item)==set(HEADER) and all(isinstance(v,str) for v in item.values()),'cell row fields')
        key=(meta['frame'],(index//9)%2,index//18,index%9)
        require(all(item[k]==str(v) for k,v in zip(HEADER[:4],key)),'cell key/order/duplicate')
        row=dict(zip(HEADER[:4],key))
        for k in ('network_id','cell_id'):
            require(re.fullmatch(r'0x[0-9a-f]+',item[k]) is not None and 0<int(item[k],16)<2**64,'cell nonzero unsigned hex identity')
            row[k]=int(item[k],16)
        network,cell=row['network'],row['cell']
        network_ids.setdefault(network,row['network_id']); cell_ids.setdefault((network,cell),row['cell_id'])
        require(network_ids[network]==row['network_id'] and cell_ids[network,cell]==row['cell_id'],'stable local cell/network identity')
        row.update({k:binary64(item[k]) for k in HEADER[6:]})
        require(all(bits(row[k])==bits(v) for k,v in zip(HEADER[6:11],coefficients[FAMILIES[cell]])),'cell coefficient ledger identity')
        for channel in ('l','r'): replay(row,channel)
        if index>=18:
            previous=rows[index-18]
            require(all(bits(row[k+'_'+ch])==bits(previous['next_'+k+'_'+ch]) for k in ('z1','z2') for ch in ('l','r')),'cell state source sequence')
        # Original Network source graph, using normalized keys instead of addresses.
        source={1:0,2:0,3:1,4:3,5:3,6:4,7:5,8:2}.get(cell)
        if source is not None:
            upstream=rows[index-cell+source]
            field='x' if cell==1 else 'y'
            require(all(bits(row['x_'+ch])==bits(upstream[field+'_'+ch]) for ch in ('l','r')),'cell input source graph')
        rows.append(row)
    require(len(set(network_ids.values()))==len(network_ids) and len(set(cell_ids.values()))==len(cell_ids),'distinct cell/network identities')
    return rows


def indexed_cells(meta,csv_path,index,expected,coefficients):
    require(isinstance(index,dict) and set(index)=={'metadata','records'},'cell hash index required')
    require(index['metadata']==identity(meta) and index['records']==identity(csv_path),'cell hash identity')
    return read_cells(meta,csv_path,expected,coefficients)


def require_cell_indexes(packet):
    require(isinstance(packet,dict) and isinstance(packet.get('cases'),list),'capture object/cases required')
    require(len(packet['cases'])==26,'complete original26 capture required')
    for row in packet['cases']:
        require(isinstance(row,dict) and isinstance(row.get('qualified'),dict) and isinstance(row.get('crossover_cells'),dict)
                and set(row['crossover_cells'])==set(row['qualified']),'cell hash indexes required for every history')


def safe_file(base,name):
    require(isinstance(name,str),'proof relative path required')
    path=pathlib.PurePosixPath(name)
    require(not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name,'proof path escape')
    result=base/name
    require(result.resolve().is_relative_to(base.resolve()),'proof path escape')
    return result


def coefficients():
    declaration=load(ROOT/PREDECLARATION); ledger_path=ROOT/LEDGER
    require(declaration['coefficient_ledger']==dict(path=LEDGER,**identity(ledger_path)),'original coefficient ledger hash')
    ledger=load(ledger_path)
    sets=declaration['coefficient_sets']
    require(len(sets)==6 and {(r['factor'],r['rate']) for r in sets}=={(f,r) for f in (4,64) for r in (44100,48000,96000)},'complete fixed coefficient sets')
    result={}
    for row in sets:
        original=[r for r in ledger['coefficients'] if (r['factor'],r['rate'])==(row['factor'],row['rate'])]
        require(len(original)==1 and row['binary64_hex']==original[0]['binary64_hex'],'predeclared exact coefficient arrays')
        result[row['factor'],row['rate']]={k:[binary64(v) for v in row['binary64_hex']['coefficients'][k]] for k in set(FAMILIES)}
    return result,declaration


def check_driver_flags(flags,target):
    require(target in ('RackSynthSaturationNumericalTrace','RackSynthSaturationNumericalReplay'),'selected driver target')
    expected=stability.producer_flags('RackSynthSaturationTransitionCandidate')
    variant='RACKSYNTH_HISTORY_TRACE' if target.endswith('Trace') else 'RACKSYNTH_HISTORY_RETURN_REPLAY'
    expected['PreprocessorDefinitions']=expected['PreprocessorDefinitions'].replace('AIRACK_SATURATION_TRANSITIONS=1;',
        'JUCE_STANDALONE_APPLICATION=1;_CONSOLE=1;'+variant+'=1;')
    normalize=lambda value:{k:v for k,v in value.items() if k!='AdditionalIncludeDirectories' and v is not None}
    require(normalize(flags)==normalize(expected),'original selected driver compiler/FP/ISA/runtime/definitions')


def check_selected_command(commands,source,flags):
    require(len(commands)==1,'selected compiler command extent')
    line=commands[0]
    paths=re.findall(r'(?<!\S)/(I|Fo|Fd)"([^"]+)"(?=\s|$)',line)
    require([pathlib.Path(value) for option,value in paths if option=='I']==
            [pathlib.Path(value) for value in flags['AdditionalIncludeDirectories'].split(';') if not value.startswith('%')]
            and [option for option,value in paths].count('Fo')==1 and [option for option,value in paths].count('Fd')==1
            and all((len(value)-len(value.rstrip('\\')))%2==0 for option,value in paths),'selected compiler paths/quoting')
    line=re.sub(r'(?<!\S)/(?:I|Fo|Fd)"[^"]+"(?=\s|$)','',line)
    source_pattern=r'\s+(?:'+re.escape(source)+'|'+re.escape('"'+source+'"')+r')\s*$'
    line,count=re.subn(source_pattern,'',line,flags=re.IGNORECASE)
    require(count==1,'selected compiler source input')
    # Only paths vary; reject every extra option/definition, including later overrides.
    expected='/c /nologo /W1 /WX- /diagnostics:column /O2 /Ob2'.split()
    for definition in ['_MBCS',*[value for value in flags['PreprocessorDefinitions'].split(';') if not value.startswith('%')]]:
        expected.extend(('/D','"'+definition.replace('"','\\"')+'"' if '"' in definition else definition))
    expected.extend('/EHsc /MD /GS /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /std:c++20 /external:W1 /Gd /TP /bigobj'.split())
    require(line.split()==expected,'selected compiler original options/definitions')


def check_selected(packet,proof,descriptor):
    root,build=pathlib.Path(descriptor['source_root']),pathlib.Path(descriptor['build_root'])
    targets=('RackSynthSaturationNumericalTraceCandidate','RackSynthSaturationNumericalTrace','RackSynthSaturationNumericalReplay')
    require(set(packet['selected_compile_records'])==set(targets),'selected compile inventory')
    for target in targets:
        record=stability.project_record(proof/'images-build'/(target+'.vcxproj'))
        selected=stability.observer_items(record,build,root,'Release') if target.endswith('TraceCandidate') else record['items']
        if target.endswith('TraceCandidate'):
            stability.check_compile(dict(record,items=selected),'RackSynthSaturationHistoryTraceCandidate')
        else:
            check_driver_flags(record['flags'],target)
            expected_names=['SaturationHistoryDiagnosis.cpp']+(['SaturationHistoryTrace.cpp'] if target.endswith('Trace') else [])
            require([i['path'] for i in selected]==[str(root/'probes/saturation/contracts'/name) for name in expected_names],'selected driver source inventory')
            require(all(not i['properties'] for i in selected),'selected driver per-source properties')
        # Capture proof paths include the images-build prefix; keep their actual producer paths.
        names=[name[len('images-build/'):] for name in packet['proof'] if name.startswith('images-build/')]
        logs=stability.compiled_logs(names,proof/'images-build',target,'Release')
        commands=stability.tlog_records(logs['CL.command.1.tlog']); reads=stability.tlog_records(logs['CL.read.1.tlog'])
        require(packet['selected_compile_records'][target]==dict(project=record,selected_items=selected,commands=commands,dependencies=reads),'selected command/project identity')
        dependencies=set()
        for item in selected:
            key=item['path'].upper()
            require(key in commands and key in reads and commands[key] and reads[key],'selected compile command/read record')
            check_selected_command(commands[key],key,record['flags'])
            require('BASELINE-8B7-20261022' not in commands[key][0].upper(),'selected compiler current source')
            dependencies.update(reads[key]); dependencies.add(key)
        for name in descriptor['sources']:
            if target.endswith('TraceCandidate') and not name.endswith('/SaturationCandidate.cpp'):
                require(str(root/name).upper() in {d.upper() for d in dependencies},'selected numerical dependency')
        if target.endswith('Trace'):
            for name in ('SaturationHistoryTrace.cpp','SaturationHistoryTrace.h'):
                require(str(root/'probes/saturation/contracts'/name).upper() in {d.upper() for d in dependencies},'selected observer dependency')


def check_case_outputs(row,base):
    h=trace.helpers(); proof=base/'proof'
    factor,rate,case=row['factor'],row['rate'],row['case']; roles=h.histories(case); inp=pathlib.Path(row['input'])
    require(set(row['qualified'])==set(row['stages'])==set(roles),'complete qualified histories')
    out=base/f'{factor}-{rate}-{case}'; start=4096+2*rate; fade=math.ceil(.020*rate); total=start+fade+128
    require(load(out/'report.json')==row,'per-case report identity')
    require(isinstance(row['commands'],list) and len(row['commands'])==2,'complete native command receipts')
    streams={}
    for role,command in zip(('replay','trace'),row['commands']):
        require(isinstance(command,dict) and set(command)=={'argv','start_utc','end_utc','exit'} and type(command['exit']) is int and command['exit']==0,'successful native command receipt')
        timestamps=[datetime.datetime.fromisoformat(command[k]) for k in ('start_utc','end_utc')]
        require(all(t.tzinfo is not None and t.utcoffset()==datetime.timedelta(0) for t in timestamps) and timestamps[0]<=timestamps[1],'native ordered UTC receipt')
        argv=command['argv']; require(isinstance(argv,list) and len(argv)==17 and pathlib.Path(argv[0]).is_absolute(),'native command shape')
        require(argv==[argv[0],'--wait-seconds','1800','--run-seconds','600','--',str(proof/(role+'.exe')),
            '--factor',str(factor),'--rate',str(rate),'--case',case,'--input',str(inp),'--output',str(out/role)],'native selected image/case/timeline command')
        require(load(out/(role+'.command.json'))==command and (out/(role+'.log')).is_file(),'native command receipt/log identity')
        manifest=dict(schema=1,role='return-'+role,completed=True,action=stability.REPLAY if role=='replay' else trace.ACTION,
            factor=factor,rate=rate,case=case,prefix=4096,common=2*rate,fade=fade,settled=128,input=inp.as_posix())
        require(load(out/role/'capture.json')==manifest,'native complete timeline manifest')
    for role in roles:
        name=f'{case}.{role}.output.f64'
        a,b=(out/'replay'/name).read_bytes(),(out/'trace'/name).read_bytes()
        receipt=stability.qualify_return(a,b,rate)
        require(row['qualified'][role]==receipt,'complete observed/unobserved audio hash identity')
        streams[role]=h.decode(a,total); h.decode(b,total)
        require(identity(out/'trace'/(name+'.stages.json'))==row['stages'][role]['raw'],'stage raw hash identity')
    require(row['original_complete_gate']==h.paired(streams,start,total,'current original complete fade+128'),'original full gate identity')
    return len(roles),sum(q['frames'] for q in row['qualified'].values())


def check_retained_proof(packet,proof):
    require(isinstance(packet['proof'],dict) and packet['proof'],'capture proof inventory')
    inventory={p.relative_to(proof).as_posix() for p in proof.rglob('*') if p.is_file()}
    require(inventory==set(packet['proof']),'complete retained proof inventory')
    for name,bound in packet['proof'].items(): require(identity(safe_file(proof,name))==bound,'retained proof hash identity: '+name)
    descriptor=packet['descriptor']['descriptor']
    record=descriptor['project']['compile']
    logs=stability.compiled_logs(descriptor['compiled_tlogs'],proof/'source-tlogs',descriptor['target'],descriptor.get('configuration','Release'))
    commands=stability.tlog_records(logs['CL.command.1.tlog'])
    for item in record['items']:
        key=item['path'].upper()
        check_selected_command(commands[key],key,record['flags'])
    stability.check_capture_descriptor(packet['descriptor'],proof)
    check_selected(packet,proof,descriptor)


def qualify_capture(path):
    """All 26/63 complete streams and their producer proof precede any cell parse."""
    require(path.is_absolute() and path.is_file(),'absolute capture JSON required')
    packet=load(path); require_cell_indexes(packet)
    require(packet['schema']=='saturation-current-capture-v1' and packet['case_count']==26 and packet['stream_count']==63
            and packet['cpu']=='DEFERRED' and packet['selected_factor'] is None and packet['adoptable'] is False
            and packet['production']=='FORCED_BYPASS','qualified current capture schema/status')
    base=path.parent; proof=base/'proof'
    check_retained_proof(packet,proof)
    descriptor=packet['descriptor']['descriptor']
    require(descriptor.get('configuration','Release')=='Release' and pathlib.Path(descriptor['source_root']).is_absolute()
            and pathlib.Path(descriptor['build_root']).is_absolute(),'explicit original source/build roots')
    require(load(proof/'source-descriptor.json')==descriptor,'retained source descriptor copy')
    require(set(packet['descriptor']['images'])=={'trace','replay'},'current trace/replay image identities')
    for role in ('trace','replay'):
        require(packet['descriptor']['images'][role]['copy']==role+'.exe','selected image role')
    candidate=proof/'source/probes/saturation/SaturationCandidate.cpp'; original=candidate.read_bytes()
    require(identity(candidate)['sha256']==CANDIDATE_SHA,'original unchanged Candidate source')
    require(trace.recover((proof/'SaturationCandidateObserved.cpp').read_bytes())==original
            and (proof/'SaturationCandidateObserved.original.cpp').read_bytes()==original
            and (proof/'SaturationCandidateObserved.recovered.cpp').read_bytes()==original,'generated observer original byte recovery')
    recovery=load(proof/'SaturationCandidateObserved.recovery.json')
    require(recovery['generated']==identity(proof/'SaturationCandidateObserved.cpp') and recovery['original']==identity(candidate)
            and recovery['recovered']==identity(proof/'SaturationCandidateObserved.recovered.cpp')
            and recovery['descriptor']==stability.json_identity(descriptor),'generated recovery receipt')
    for name in (stability.TOOL,'tools/saturation_history_trace.py','probes/saturation/contracts/SaturationHistoryTrace.cpp',
                 'probes/saturation/contracts/SaturationHistoryTrace.h','probes/saturation/contracts/SaturationHistoryCases.h',
                 'probes/saturation/contracts/SaturationReturnOracle.h'):
        require(identity(proof/'source'/name)==identity(ROOT/name),'qualified current observer/oracle source identity: '+name)
    require(stability.generate_observer(original,descriptor,proof/'source')==(proof/'SaturationCandidateObserved.cpp').read_bytes(),'current generated callback identity')
    coeffs,declaration=coefficients()
    for source in declaration['sources_before_experiment']:
        require(identity(proof/'source'/source['path'])=={k:source[k] for k in ('bytes','sha256')},'original numerical source invariance')
    fixed_path=ROOT/stability.PACKET; fixed=load(fixed_path)
    require(packet['original_packet']==identity(fixed_path),'original26 source packet identity')
    key=lambda r:(r['factor'],r['rate'],r['case'])
    require([key(r) for r in packet['cases']]==[key(r) for r in fixed['cases']] and len({key(r) for r in packet['cases']})==26,'fixed original26 case order/inventory')
    h=trace.helpers(); qualified_count=0; stereo_frames=0
    for row,canonical in zip(packet['cases'],fixed['cases']):
        factor,rate,case=key(row); inp=pathlib.Path(row['input'])
        require(row['input']==canonical['input'] and row['input_files']==canonical['input_files']
                and h.retained(inp,factor,rate,case)==row['input_files'],'fixed retained corpus source/hash binding')
        count,frames=check_case_outputs(row,base)
        qualified_count+=count; stereo_frames+=frames
    require(qualified_count==63 and packet['stereo_frames']==stereo_frames,'complete original26/63 audio qualification')
    return packet,coeffs


def serializable(value):
    if isinstance(value,Fraction): return str(value)
    if isinstance(value,float): return value.hex()
    if isinstance(value,dict): return {k:serializable(v) for k,v in value.items()}
    if isinstance(value,(list,tuple)): return [serializable(v) for v in value]
    return value


def publish(directory,result):
    require(directory.is_absolute() and not directory.exists(),'fresh absolute analysis directory required')
    directory.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.crossover-',dir=directory.parent) as temporary:
        staged=pathlib.Path(temporary)/'complete'; staged.mkdir()
        stability.write_json(staged/'analysis.json',serializable(result))
        staged.rename(directory)


def analyze(directory,capture):
    require(directory.is_absolute() and not directory.exists(),'fresh absolute analysis directory required')
    packet,coeffs=qualify_capture(capture)
    cases=[]
    for row in packet['cases']:
        factor,rate,case=row['factor'],row['rate'],row['case']; data={}
        mode=int(case.split('-')[1])
        source_mode=1-mode if case.startswith('mode-') else mode
        for role in trace.helpers().histories(case):
            history={'old0':0,'old1':1,'fresh':2}[role]
            expected=dict(schema='saturation-crossover-cells-v1',status='complete',factor=factor,rate=rate,history=history,
                frame=4096+2*rate-64,source_mode=source_mode,endpoints=1,records=1152 if factor==64 and source_mode==1 else 0,cell_order=ORDER)
            stem=capture.parent/f'{factor}-{rate}-{case}'/'trace'/f'{case}.{role}.output.f64'
            data[role]=indexed_cells(pathlib.Path(str(stem)+'.cells.json'),pathlib.Path(str(stem)+'.cells.csv'),row['crossover_cells'][role],expected,coeffs[factor,rate])
        pairs={}
        for role in tuple(data)[1:]:
            left,right=data[role],data['old0']; require(len(left)==len(right),'paired cell key extent')
            witnesses=[]; first=None; maximum=None; max_size=Fraction(-1)
            for a,b in zip(left,right):
                key=[a[k] for k in HEADER[:4]]
                require(key==[b[k] for k in HEADER[:4]],'normalized paired cell key identity')
                for channel in ('l','r'):
                    proof=paired_identity(a,b,channel)
                    witness=dict(key=key,channel=channel,observed_left=a,observed_right=b,**proof)
                    witnesses.append(witness)
                    size=max(map(abs,proof['forcing']))
                    if size and first is None: first=witness
                    if size>max_size: max_size=size; maximum=witness
            pairs[role]=dict(scalar_updates=len(witnesses),first_nonzero_forcing=first,maximum_forcing=maximum,
                             maximum_absolute_forcing=str(max_size if witnesses else 0),all_exact_pair_identities=True,
                             witnesses=witnesses,conclusion='local additive forcing observed' if first else 'no local evidence of additive forcing')
        cases.append(dict(factor=factor,rate=rate,case=case,source_mode=source_mode,pairs=pairs))
    result=dict(schema='saturation-crossover-witness-v1',status='complete',capture=dict(path=str(capture),**identity(capture)),
        qualified_cases=26,qualified_streams=63,cases=cases,predeclaration=identity(ROOT/PREDECLARATION),coefficient_ledger=identity(ROOT/LEDGER),
        arithmetic='separate binary64 replay including signed zero; exact Fraction operands and paired state identities',
        cpu='DEFERRED',timing='NOT_RUN',heap='NOT_RUN',cpu_output_integrity='NOT_RUN',selected_factor=None,adoptable=False,
        production='FORCED_BYPASS',open_gates=['R4','R5','R6','R7'],acceptance_gates='UNCHANGED')
    publish(directory,result)
    return result


class Parser(argparse.ArgumentParser):
    def error(self,message): raise ValueError(message)


def main():
    try:
        parser=Parser(description=__doc__,allow_abbrev=False,add_help=False)
        parser.add_argument('--analyze',required=True,type=pathlib.Path)
        parser.add_argument('--capture',required=True,type=pathlib.Path)
        args=parser.parse_args()
        analyze(args.analyze,args.capture)
        print('crossover diagnostic complete; CPU DEFERRED; gates unchanged')
        return 0
    except (ValueError,OSError,KeyError,TypeError,AttributeError,IndexError,OverflowError,csv.Error,ET.ParseError) as error:
        print('crossover diagnostic refused: '+str(error).replace('\n',' '),file=sys.stderr)
        return 2


if __name__=='__main__': sys.exit(main())
