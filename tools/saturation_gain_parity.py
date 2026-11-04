"""Exact scalar gain matrix, strict capture reader and MP diagnostics."""
import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile
import os
import stat
import xml.etree.ElementTree as ET

from saturation_gain_import import plain_output, validate_assembly, inspect_pe, inspect_caller

ROOT=Path(__file__).resolve().parents[1]
SOURCE_FILES=('CMakeLists.txt','tools/saturation_gain_import.py','tools/saturation_gain_parity.py',
              'probes/saturation/SaturationGainImport.asm',
              'probes/saturation/SaturationGain.h','probes/saturation/SaturationGainRuntime.h',
              'probes/saturation/contracts/SaturationGainParity.cpp',
              'probes/saturation/contracts/SaturationGainLinkTests.cpp')
PINS={'candidate-20261021':'e0d1592a45083191d3e3d14f2548ec4b032c5fed0e0109d14969859917953599',
      'candidate-20261023-stable-v2':'30fc96fc8b4e0f1e9708786e9cf6ae80af42212adb42f7105f4620c8e51cd588'}

def require(ok,message):
    if not ok:raise ValueError(message)
def identity(raw):return dict(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())
def word(value):return struct.pack('>d',value).hex()
def number(bits):
    require(isinstance(bits,str) and re.fullmatch('[0-9a-f]{16}',bits),'sixteen lowercase hex digits required')
    value=struct.unpack('>d',bytes.fromhex(bits))[0]
    require(math.isfinite(value),'nonfinite finite-domain result')
    return value
def import_digest():
    raw=(ROOT/'probes/saturation/SaturationGainImport.asm').read_bytes()
    validate_assembly(raw)
    return identity(raw)['sha256']

def gain_inputs() -> list:
    rows=[]
    def add(kind,value):rows.append(dict(id=len(rows),kind=kind,db=word(value)))
    for n in range(65537):add('grid',-48+96*n/65536)
    for n in range(768):add('center',-48+(n+.5)/8)
    for n in range(769):
        x=-48+n/8
        for v in (math.nextafter(x,-math.inf),x,math.nextafter(x,math.inf)):add('seam',v)
    rng=random.Random(0x53415455)
    for _ in range(4096):add('float32',struct.unpack('<f',struct.pack('<f',rng.uniform(-48,48)))[0])
    add('zero',0.);add('zero',-0.)
    x=number('403424d560000000')
    for v in (math.nextafter(x,-math.inf),x,math.nextafter(x,math.inf)):add('witness',v)
    for x in (-48.,-24.,-18.,-6.,-3.,0.,3.,6.,12.,18.,24.,36.,48.):
        for v in (math.nextafter(x,-math.inf),x,math.nextafter(x,math.inf)):add('endpoint',v)
    return rows

def input_bytes():return ('id,db\n'+''.join(f"{r['id']},{r['db']}\n" for r in gain_inputs())).encode('ascii')

def rne_word(value):
    """Round an exact Fraction to binary64 with integer ties-to-even, including subnormals."""
    if not value:return word(0.)
    sign=int(value<0);value=abs(value);n,d=value.numerator,value.denominator
    exponent=n.bit_length()-d.bit_length()
    if exponent>=0:
        if n<d<<exponent:exponent-=1
    elif n<<(-exponent)<d:exponent-=1
    shift=max(exponent-52,-1074)
    if shift>=0:q,r=divmod(n,d<<shift);denominator=d<<shift
    else:q,r=divmod(n<<(-shift),d);denominator=d
    if 2*r>denominator or (2*r==denominator and q&1):q+=1
    if q==2**53:q>>=1;exponent+=1
    if exponent < -1022:bits=q
    else:bits=((exponent+1023)<<52)|(q-2**52)
    return f'{bits|(sign<<63):016x}'

def exponent_word(db):
    value=number(db)
    return db if value==0 else rne_word(Fraction(value)/20)

def validate_rows(raw,inputs):
    require(raw.endswith(b'\n') and b'\r' not in raw,'complete LF rows required')
    lines=raw.decode('ascii').splitlines()
    require(lines[0]=='id,db,exponent,original,canonical' and len(lines)==len(inputs)+1,'row header/extent mismatch')
    result=[]
    for expected,line in zip(inputs,lines[1:]):
        fields=line.split(',')
        require(len(fields)==5,'row width')
        rid,db,exponent,original,canonical=fields
        require(rid==str(expected['id']) and db==expected['db'],'row identity/order mismatch')
        for bits in (db,exponent,original,canonical):number(bits)
        require(exponent==exponent_word(db),'division by20 is not exact Fraction RNE')
        result.append(dict(id=expected['id'],db=db,exponent=exponent,original=original,canonical=canonical))
    return result

def source_descriptor():
    files={name:identity((ROOT/name).read_bytes()) for name in SOURCE_FILES}
    for folder,pin in PINS.items():
        name=f'probes/saturation/contracts/fixtures/{folder}/manifest.json'
        info=identity((ROOT/name).read_bytes());require(info['sha256']==pin,'external fixture pin changed')
        files[name]=info
    return dict(schema=1,files=files)

def validate_identity(receipt,profile,source):
    require(receipt['profile']==profile,'wrong native profile')
    require(receipt['source']==source,'wrong source identity')
    require(receipt['import_sha256']==import_digest(),'wrong import identity')

def compare_rows(release,debug):
    require(len(release)==len(debug),'cross-profile extent')
    for a,b in zip(release,debug):
        require((a['id'],a['db'],a['exponent'])==(b['id'],b['db'],b['exponent']),'cross-profile row identity')
        require(a['original']==a['canonical'],'Release canonical changed original bits')
        require(a['canonical']==b['canonical'],'canonical cross-profile mismatch')

def boundary_cases(root,fixture):
    root=Path(root)
    values=(('source',root/'tools/gain-boundary-must-not-exist',False),
            ('source-root',root,False),
            ('parent-traversal',root/'build/../docs/gain-boundary-must-not-exist',False),
            ('case-source',Path(str(root).upper())/'docs/gain-boundary-must-not-exist',False),
            ('redirected',Path(fixture['link'])/'nested/gain-boundary-must-not-exist',False),
            ('redirected-parent',Path(fixture['link'])/'../gain-boundary-must-not-exist',False),
            ('build-root',root/'build',False),
            ('build-child',root/'build/gain-boundary-must-not-exist',True),
            ('external-child',Path(fixture['external_output']),True))
    return [dict(name=name,path=path.as_posix(),accepted=accepted) for name,path,accepted in values]

def fixture_information(path,reparse=False):
    # Stable Windows identity excludes last-access time, which a read may update.
    import ctypes
    from ctypes import wintypes
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateFileW.argtypes=(wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,
                                wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE)
    kernel.CreateFileW.restype=wintypes.HANDLE
    kernel.GetFileInformationByHandle.argtypes=(wintypes.HANDLE,ctypes.c_void_p)
    kernel.CloseHandle.argtypes=(wintypes.HANDLE,)
    handle=kernel.CreateFileW(str(path),0,7,None,3,0x02000000|(0x00200000 if reparse else 0),None)
    require(handle!=ctypes.c_void_p(-1).value,'boundary fixture handle failed')
    data=(wintypes.DWORD*13)()
    try:require(kernel.GetFileInformationByHandle(handle,data),'boundary fixture information failed')
    finally:kernel.CloseHandle(handle)
    keys=('attributes','creation_low','creation_high',None,None,'write_low','write_high',
          'volume','size_high','size_low','links','index_high','index_low')
    return {key:int(value) for key,value in zip(keys,data) if key}

def boundary_fixture_identity(fixture):
    link,target=Path(fixture['link']),Path(fixture['target'])
    require(link.is_junction() and not target.is_junction() and not target.is_symlink(),
            'prepared boundary junction/target required')
    require(link.resolve()==target.resolve(),'boundary junction target changed')
    require(not any(target.iterdir()),'boundary target must remain empty')
    return dict(link=link.as_posix(),target=target.as_posix(),external_output=fixture['external_output'],
                link_information=fixture_information(link,True),target_information=fixture_information(target),
                target_members=[])

def validate_boundary_proof(proof,fixture,root):
    require(set(proof)=={'cases','fixture_before','fixture_after'},'boundary proof fields')
    require(proof['cases']==boundary_cases(root,fixture),'native output-boundary behavior failed/incomplete')
    require(proof['fixture_before']==fixture and proof['fixture_after']==fixture,
            'boundary fixture changed before/during capture')

def prepare(directory,archive,obj):
    directory=Path(directory)
    plain_output(directory/'boundary-check-must-not-exist')
    descriptor=source_descriptor()
    import_digest()
    require(obj.is_file() and archive.is_file(),'actual native object/archive required')
    fixture_dir=directory/'boundary-fixture'
    plain_output(fixture_dir/'setup-must-not-exist')
    fixture_dir.mkdir(parents=True,exist_ok=True)
    target=fixture_dir/'target';target.mkdir(exist_ok=True)
    link=fixture_dir/'redirect'
    if not link.exists():
        command=['cmd','/c','mklink','/J',str(link),str(target)]
        print(json.dumps(dict(boundary_fixture_command=command)),flush=True)
        subprocess.run(command,check=True)
    fixture=boundary_fixture_identity(dict(link=link.as_posix(),target=target.as_posix(),
        external_output=(Path(tempfile.gettempdir())/('airack-gain-boundary-'+directory.name+'-must-not-exist')).as_posix()))
    data=dict(source=descriptor,inputs=identity(input_bytes()),import_sha256=import_digest(),archive=identity(archive.read_bytes()),
              object=identity(obj.read_bytes()),object_path=obj.as_posix(),archive_path=archive.as_posix(),root=ROOT.as_posix(),
              boundary_fixture=fixture)
    raw=(json.dumps(data,sort_keys=True,indent=2)+'\n').encode()
    directory.mkdir(parents=True,exist_ok=True)
    # CMake reruns only when a producer changed. Never bless a stale descriptor silently.
    for name,content in (('source.json',raw),('GainParitySource.h',
          ('#pragma once\n#define GAIN_SOURCE_SHA "'+identity(raw)['sha256']+'"\n'
           '#define GAIN_SOURCE_FILE "'+(directory/'source.json').as_posix()+'"\n').encode())):
        path=directory/name
        if path.exists():require(path.read_bytes()==content,'stale generated source; use a reviewed fresh boundary')
        else:path.write_bytes(content)

def validate_capture_members(directory,files,source):
    required={'image.exe','private.obj','private.lib','provider.dll','inputs.csv','rows.csv','centers.csv'}
    required.update('source/'+name for name in source['files'])
    require(set(files)==required,'exact complete archived member set required')
    for name in files:
        require('\\' not in name and ':' not in name and not Path(name).is_absolute() and '..' not in Path(name).parts,
                'unsafe receipt member')
    present={p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    require(present==required|{'receipt.json','source.json'},'unexpected/missing archived member')
    saved={}
    for name,info in files.items():
        path=directory/name
        for ancestor in (path,*path.parents):
            if ancestor==directory.parent:break
            require(not ancestor.is_symlink() and not ancestor.is_junction(),'redirected capture member')
        raw=path.read_bytes();require(identity(raw)==info,'capture file changed: '+name);saved[name]=raw
    return saved

def read_capture(directory,profile):
    """Reread every bound byte before interpreting any numerical row."""
    directory=Path(directory);receipt_raw=(directory/'receipt.json').read_bytes()
    receipt=json.loads(receipt_raw)
    descriptor_raw=(directory/'source.json').read_bytes();descriptor=json.loads(descriptor_raw)
    validate_identity(receipt,profile,descriptor['source'])
    require(receipt['source_sha256']==identity(descriptor_raw)['sha256'],'compiled source descriptor mismatch')
    require(descriptor['import_sha256']==import_digest(),'descriptor import mismatch')
    require(descriptor['source']==source_descriptor(),'current producer differs from captured source')
    saved=validate_capture_members(directory,receipt['files'],descriptor['source'])
    for name,info in descriptor['source']['files'].items():
        require(identity(saved['source/'+name])==info,'archived source differs')
    require(saved['inputs.csv']==input_bytes(),'complete exact input matrix required')
    require(identity(saved['private.obj'])==descriptor['object'],'archived import object changed')
    require(identity(saved['private.lib'])==descriptor['archive'],'archived import library changed')
    inspect_caller(saved['image.exe'],inspect_pe(saved['image.exe']))
    require(receipt['source_sha256'].encode('ascii') in saved['image.exe'],'image lacks compiled source binding')
    require(identity(saved['provider.dll'])=={k:receipt['provider'][k] for k in ('bytes','sha256')},'archived provider changed')
    require(identity(Path(receipt['provider']['path']).read_bytes())==identity(saved['provider.dll']),'loaded provider file changed')
    require(receipt['fresh_thread_cpp_allocations']==0,'fresh-thread C++ allocation observed')
    fixture=descriptor['boundary_fixture']
    require(boundary_fixture_identity(fixture)==fixture,'prepared boundary fixture changed')
    validate_boundary_proof(receipt['output_boundary'],fixture,Path(descriptor['root']))
    for name,raw in saved.items():require((directory/name).read_bytes()==raw,'capture changed during complete reread')
    require((directory/'receipt.json').read_bytes()==receipt_raw and (directory/'source.json').read_bytes()==descriptor_raw,'receipt changed during read')
    require(boundary_fixture_identity(fixture)==fixture,'boundary fixture changed during reread')
    return validate_rows(saved['rows.csv'],gain_inputs()),saved,receipt

def mp_diagnostics(rows):
    from decimal import Context, Decimal, ROUND_HALF_EVEN, localcontext
    low,high=(Context(prec=p,rounding=ROUND_HALF_EVEN) for p in (120,180))
    max_convergence=Decimal(0);errors={'original':Decimal(0),'canonical':Decimal(0)}
    witness=None
    # Repeated inputs retain distinct row identities; exact exponent diagnostics may be reused.
    cache={}
    # Decimal power agreement is convergence evidence, not a formal interval/rounding proof.
    with localcontext(Context(prec=210,rounding=ROUND_HALF_EVEN)):
        for row in rows:
            bits=row['exponent']
            if bits not in cache:
                exponent=number(bits);exact=Decimal.from_float(exponent)
                require(Fraction(exact)==Fraction.from_float(exponent),'inexact binary64 exponent conversion')
                a=low.power(Decimal(10),exact);b=high.power(Decimal(10),exact)
                convergence=abs(a-b);max_convergence=max(max_convergence,convergence)
                require(convergence<=Decimal('1e-60'),'MP120/180 did not converge')
                cache[bits]=b
            reference=cache[bits]
            report={}
            for field in errors:
                error=Decimal.from_float(number(row[field]))-reference
                errors[field]=max(errors[field],abs(error));report[field+'_signed_error']=format(error,'.85g')
            if row['db']=='403424d560000000':witness=dict(row,**report,mp_nearest=word(float(reference)))
        return dict(precisions=[120,180],unique_exponents=len(cache),
                    convergence_max=format(max_convergence,'.85g'),convergence_limit='1e-60',
                    max_absolute_error={k:format(v,'.85g') for k,v in errors.items()},witness=witness,
                    gain_error_tolerance=None)

def compare(release,debug):
    a,sa,ra=read_capture(release,'Release');b,sb,rb=read_capture(debug,'Debug')
    compare_rows(a,b)
    centers=[r for r,i in zip(a,gain_inputs()) if i['kind']=='center']
    expected=('id,db,original\n'+''.join(f"{i},{r['db']},{r['canonical']}\n" for i,r in enumerate(centers))).encode()
    require(sa['centers.csv']==expected,'canonical centers differ from actual original Release GainCurve')
    witness=[r for r in b if r['db']=='403424d560000000']
    require(len(witness)==1 and witness[0]['original']=='40245584c36bbe71' and witness[0]['canonical']=='40245584c36bbe72','known Debug caller-isolation witness changed')
    return dict(schema=1,status='PASS',rows=len(a),canonical_bit_mismatches=0,release_original_bit_mismatches=0,
                actual_original_release_centers=768,division='exact Fraction round-to-nearest ties-to-even',
                release_mp=mp_diagnostics(a),debug_mp=mp_diagnostics(b),
                allocation_visibility='instrumented C++ new/new[] only; CRT-internal heaps not observed',
                source=ra['source'],import_sha256=import_digest(),CPU='DEFERRED',adoptable=False)

def _plain(path):
    path=Path(path)
    require(path.is_absolute() and '..' not in path.parts and path==path.resolve(),'plain canonical absolute path required')
    require(all(not p.is_symlink() and not p.is_junction() for p in (path,*path.parents)),'redirected producer path')
    return path

def _bytes(path):
    path=_plain(path)
    require(path.is_file() and stat.S_ISREG(path.stat().st_mode),'producer regular file missing: '+str(path))
    return path.read_bytes()

def _json(raw):
    def unique(pairs):
        value={}
        for k,v in pairs:
            require(k not in value,'duplicate producer JSON member');value[k]=v
        return value
    try:return json.loads(raw,object_pairs_hook=unique)
    except (UnicodeError,json.JSONDecodeError) as exc:raise ValueError('malformed producer receipt') from exc

def _cache(build,root,read=_bytes):
    text=read(build/'CMakeCache.txt').decode('utf-8')
    def field(name):
        values=re.findall('^'+name+r':[^=]+=(.*)$',text,re.MULTILINE)
        require(len(values)==1,'configured producer field missing/duplicate: '+name);return values[0].strip()
    require(_plain(Path(field('CMAKE_HOME_DIRECTORY')))==root,'producer source root contradiction')
    require(field('CMAKE_GENERATOR')=='Visual Studio 17 2022' and field('CMAKE_GENERATOR_PLATFORM')=='x64','MSVC x64 producer required')
    return _plain(Path(field('CMAKE_GENERATOR_INSTANCE')))

def _logs(build,target,profile,name,retained=None):
    from saturation_numerical_stability import tlog_records
    selected=([Path(p) for p in retained if Path(p).name==name and Path(p).is_relative_to(build/(target+'.dir')/profile)]
              if retained is not None else list((build/(target+'.dir')/profile).rglob(name)))
    require(len(selected)==1,'selected producer record missing/duplicate: '+target+'/'+name)
    path=selected[0] if retained is None else retained[str(selected[0])]
    _bytes(path);return selected[0],tlog_records(path)

def _values(records):return {v.upper() for values in records.values() for v in values}

def _gain_import(build,profile,root,retained=None):
    """Fixed import build roles; configured MASM command, actual read/write and LIB records."""
    from saturation_numerical_stability import project_record,NS
    require(profile in ('Debug','Release'),'gain import profile required')
    def read(path):
        require(retained is None or str(path) in retained,'retained gain import role missing: '+str(path))
        return _bytes(path if retained is None else retained[str(path)])
    instance=_cache(build,root,read);target='RackSynthSaturationGainImport';paths={build/'CMakeCache.txt'}
    project=build/(target+'.vcxproj');paths.add(project);tree=ET.fromstring(read(project))
    groups=[g for g in tree.findall('ms:ItemDefinitionGroup',NS) if f"=='{profile}|x64'" in g.get('Condition','')]
    require(len(groups)==1,'selected import configuration required')
    masm=groups[0].find('ms:MASM',NS)
    require(masm is not None and masm.findtext('ms:GenerateDebugInformation',namespaces=NS)=='false' and
            'CMAKE_INTDIR="'+profile+'"' in masm.findtext('ms:PreprocessorDefinitions','',NS),'configured MASM profile drift')
    asm=root/'probes/saturation/SaturationGainImport.asm';paths.add(asm);validate_assembly(read(asm))
    items=tree.findall('.//ms:MASM[@Include]',NS)
    require(len(items)==1 and Path(items[0].get('Include'))==asm and not list(items[0]),'fixed MASM source selection required')
    def output(name):
        fields=[n.text for n in tree.findall('.//ms:'+name,NS) if f"=='{profile}|x64'" in n.get('Condition','')]
        require(len(fields)==1,'selected import output required: '+name);return fields[0]
    archive=_plain(Path(output('OutDir'))/(output('TargetName')+output('TargetExt')))
    require(archive==build/profile/(target+'.lib'),'gain import archive output contradiction');paths.add(archive)
    obj=build/(target+'.dir')/profile/'SaturationGainImport.obj';paths.add(obj)
    metadata=([Path(p) for p in retained if Path(p).name=='CMakeASM_MASMCompiler.cmake' and Path(p).is_relative_to(build/'CMakeFiles')]
              if retained is not None else list((build/'CMakeFiles').glob('*/CMakeASM_MASMCompiler.cmake')))
    require(len(metadata)==1,'configured ASM tool identity required');paths.add(metadata[0]);text=read(metadata[0]).decode()
    for key in ('CMAKE_ASM_MASM_COMPILER','CMAKE_AR','CMAKE_LINKER'):
        values=re.findall(r'set\('+key+r' "([^"\n]+)"\)',text)
        require(len(values)==1,'configured native tool required: '+key);paths.add(_plain(Path(values[0])))
    rules=instance/'MSBuild/Microsoft/VC/v170/BuildCustomizations'
    props=rules/'masm.props';targets=rules/'masm.targets';paths.update((props,targets))
    require(b'ml64.exe /c [AllOptions] [AdditionalOptions] /Ta[Inputs]' in read(props) and
            b'_WriteMasmTlogs' in read(targets) and b'_MASM' in read(targets),'configured MASM rule missing')
    for name in ('Masm.read.1u.tlog','Masm.write.1u.tlog','Lib.command.1.tlog','Lib-link.read.1.tlog','Lib-link.write.1.tlog'):
        path,records=_logs(build,target,profile,name,retained);paths.add(path)
        if name.startswith('Masm'):
            require(set(records)=={str(asm).upper()},'actual MASM source key required')
            if name.startswith('Masm.write'):require(_values(records)=={str(obj).upper()},'actual MASM object output required')
        elif name=='Lib.command.1.tlog':
            command=' '.join(v for values in records.values() for v in values).upper()
            require(str(archive).upper() in command and '/MACHINE:X64' in command and
                    str(obj.relative_to(build)).upper() in command,'actual import librarian command required')
        elif name.startswith('Lib-link.read'):
            require(str(obj).upper() in _values(records),'actual import librarian input required');paths.update(Path(v) for v in _values(records))
        else:require(str(archive).upper() in _values(records),'actual import archive output required')
    if retained is None:require(asm.stat().st_mtime_ns<=obj.stat().st_mtime_ns<=archive.stat().st_mtime_ns,'import object/archive predates source')
    return paths,archive

def gain_import_inputs(build,profile,source_root):
    """Shared fixed import evidence, separate from C++ source/read inventories."""
    build,root=_plain(build),_plain(source_root)
    paths,_=_gain_import(build,profile,root)
    paths.update(root/name for name in ('tools/saturation_gain_parity.py','tools/saturation_gain_import.py','tools/saturation_numerical_stability.py','probes/saturation/SaturationGainRuntime.h'))
    return {str(p):identity(_bytes(p)) for p in sorted(paths)}

def check_retained_gain_import(inputs,build,profile,root,retained):
    """Recheck the same fixed import roles using the retained quality/numerical files."""
    paths,_=_gain_import(Path(build),profile,Path(root),retained)
    paths.update(Path(root)/name for name in ('tools/saturation_gain_parity.py','tools/saturation_gain_import.py','tools/saturation_numerical_stability.py','probes/saturation/SaturationGainRuntime.h'))
    require(set(inputs)==set(retained) and {str(p) for p in paths}==set(inputs),'complete retained import roles required')
    require(all(identity(_bytes(retained[p]))==bound for p,bound in inputs.items()),'retained import dependency changed')

def _fixture_producer_records(native,root):
    from saturation_numerical_stability import project_record,numerical_sources
    native,root=_plain(native),_plain(root);consumer='RackSynthSaturationFixtureDiagnosis';candidate='RackSynthSaturationCandidate'
    require(native.name==consumer+'.exe' and native.parent.name in ('Debug','Release') and
            native.parent.parent.name==consumer+'_artefacts','exact normal Diagnosis image required')
    profile=native.parent.name;build=native.parents[2];_cache(build,root)
    paths,archive=_gain_import(build,profile,root);paths.update((native,root/'CMakeLists.txt'))
    paths.update(root/name for name in numerical_sources(root))
    paths.update(root/name for name in ('tools/saturation_gain_parity.py','tools/saturation_gain_import.py','tools/saturation_numerical_stability.py','probes/saturation/SaturationGainRuntime.h','probes/saturation/contracts/SaturationFixtureTests.cpp'))
    compiler=list((build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    require(len(compiler)==1,'selected C++ compiler metadata required');paths.add(compiler[0])
    tools=re.findall(r'set\(CMAKE_CXX_COMPILER "([^"\n]+)"\)',_bytes(compiler[0]).decode())
    require(len(tools)==1,'selected C++ compiler identity required');paths.add(_plain(Path(tools[0])))
    objects={}
    for target,names in ((candidate,('SaturationCandidate.cpp','SaturationWideNetwork.cpp')),(consumer,('contracts/SaturationFixtureTests.cpp',))):
        project=build/(target+'.vcxproj');paths.add(project);_bytes(project);record=project_record(project,profile)
        sources=[root/'probes/saturation'/name for name in names]
        require([Path(i['path']) for i in record['items']]==sources and all(not i['properties'] for i in record['items']),'exact normal producer translation units required')
        flags=record['flags'];require(flags.get('RuntimeLibrary')==('MultiThreadedDLL' if profile=='Release' else 'MultiThreadedDebugDLL') and
            flags.get('Optimization')==('MaxSpeed' if profile=='Release' else 'Disabled') and flags.get('LanguageStandard')=='stdcpp20','ordinary producer profile settings required')
        logs={}
        for name in ('CL.command.1.tlog','CL.read.1.tlog','CL.write.1.tlog'):
            path,logs[name]=_logs(build,target,profile,name);paths.add(path)
        objects[target]=[]
        for source in sources:
            key=str(source).upper();require(key in logs['CL.command.1.tlog'] and key in logs['CL.read.1.tlog'],'actual normal selected compile required')
            command=' '.join(logs['CL.command.1.tlog'][key]).upper()
            options=('/O2 ','/MD ') if profile=='Release' else ('/OD ','/MDD ','/RTC1 ','/ZI ')
            require(all(v in command for v in (*options,'/FP:PRECISE ','/STD:C++20 ','/BIGOBJ ')) and key in command,'actual ordinary compiler options required')
            require(not any(v in command for v in ('/FP:FAST','/ARCH:','/FP:CONTRACT','AIRACK_SATURATION_TRANSITIONS','NUMERICAL_TEST','REFERENCE_TRACE','BASELINE-8B7')),'normal producer compiler variant drift')
            require(('RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS=1' in command)==(target==consumer),'normal Diagnosis compile definition required')
            deps={str(source),*logs['CL.read.1.tlog'][key]};paths.update(Path(p) for p in deps)
            if source.name=='SaturationCandidate.cpp':require(str(root/'probes/saturation/SaturationGainRuntime.h').upper() in {p.upper() for p in deps},'runtime header missing from actual compile reads')
            obj=build/(target+'.dir')/profile/(source.stem+'.obj');objects[target].append(obj);paths.add(obj)
            writes={v.upper() for k,values in logs['CL.write.1.tlog'].items() if key in k.split('|') for v in values}
            require(str(obj).upper() in writes,'actual source object output required')
            require(all(Path(p).stat().st_mtime_ns<=obj.stat().st_mtime_ns for p in deps),'compiled object predates read source')
    library=build/profile/(candidate+'.lib');paths.add(library)
    for name in ('Lib.command.1.tlog','Lib-link.read.1.tlog','Lib-link.write.1.tlog'):
        path,records=_logs(build,candidate,profile,name);paths.add(path)
        values=_values(records)
        if name=='Lib.command.1.tlog':require(str(library).upper() in ' '.join(values),'actual normal library command required')
        elif name.startswith('Lib-link.read'):
            require(all(str(obj).upper() in values for obj in objects[candidate]),'actual normal library object inputs required');paths.update(Path(p) for p in values)
        else:require(str(library).upper() in values,'actual normal library output required')
    require(all(p.stat().st_mtime_ns<=library.stat().st_mtime_ns for p in objects[candidate]),'normal library predates object')
    linked=[library,archive,*objects[consumer]]
    for name in ('link.command.1.tlog','link.read.1.tlog','link.write.1.tlog'):
        path,records=_logs(build,consumer,profile,name);paths.add(path);values=_values(records)
        if name=='link.command.1.tlog':
            command=' '.join(values);require(str(native).upper() in command and all(p.name.upper() in command for p in linked),'actual normal final link command required')
        elif name=='link.read.1.tlog':
            inputs=values|{p for k in records for p in k.split('|')}
            require(all(str(p).upper() in inputs for p in linked),'actual normal final link inputs required');paths.update(Path(p) for p in values)
        else:require(str(native).upper() in values,'actual normal final image output required')
    require(all(p.stat().st_mtime_ns<=native.stat().st_mtime_ns for p in linked),'normal image predates linked inputs')
    raw=_bytes(native);pe=inspect_pe(raw);calls=inspect_caller(raw,pe)
    files={str(p):identity(_bytes(p)) for p in sorted(paths)}
    return dict(schema='saturation-normal-fixture-producer-v1',producer=dict(target=consumer,profile=profile,source_root=str(root),build_root=str(build),image=str(native)),
                masm_command_evidence='configured installed rule; actual source/read and object/write records',image_import=pe,private_iat_call_rvas=calls,files=files)

def _fixture_producer(native,root):
    try:return _fixture_producer_records(native,root)
    except ET.ParseError as exc:raise ValueError('malformed producer project XML') from exc

def prepare_fixture_producer(native: Path,source_root: Path) -> Path:
    proof=_fixture_producer(native,source_root);build=Path(proof['producer']['build_root'])
    path=build/'saturation-gain-runtime'/proof['producer']['profile']/'RackSynthSaturationFixtureDiagnosis.json'
    _plain(path);path.parent.mkdir(parents=True,exist_ok=True)
    temporary=_plain(path.with_suffix('.tmp'))
    require(not temporary.exists(),'fresh producer temporary required')
    with temporary.open('x',encoding='utf-8') as stream:json.dump(proof,stream,indent=2);stream.write('\n')
    os.replace(temporary,path)
    return path

def fixture_producer_inputs(native: Path,source_root: Path) -> dict:
    native=_plain(native)
    require(native.name=='RackSynthSaturationFixtureDiagnosis.exe' and native.parent.name in ('Debug','Release') and native.parent.parent.name=='RackSynthSaturationFixtureDiagnosis_artefacts','exact normal Diagnosis image required')
    path=native.parents[2]/'saturation-gain-runtime'/native.parent.name/'RackSynthSaturationFixtureDiagnosis.json'
    raw=_bytes(path);proof=_json(raw);current=_fixture_producer(native,source_root)
    require(proof==current,'current normal producer receipt changed/stale/incomplete')
    return {**current['files'],str(path):identity(raw)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--write-inputs',type=Path);p.add_argument('--prepare',type=Path)
    p.add_argument('--archive',type=Path);p.add_argument('--object',type=Path)
    p.add_argument('--compare',nargs=2,type=Path);p.add_argument('--output',type=Path)
    p.add_argument('--bind-fixture-producer',type=Path)
    a=p.parse_args()
    try:
        require(sum(bool(x) for x in (a.write_inputs,a.prepare,a.compare,a.bind_fixture_producer))==1,'select exactly one action')
        if a.write_inputs:
            path=plain_output(a.write_inputs);path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(input_bytes())
        elif a.prepare:prepare(a.prepare,a.archive,a.object)
        elif a.bind_fixture_producer:prepare_fixture_producer(a.bind_fixture_producer,ROOT)
        else:
            path=plain_output(a.output);result=compare(*a.compare)
            path.parent.mkdir(parents=True,exist_ok=True)
            with path.open('x',encoding='utf-8') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    except (ValueError,OSError,KeyError) as exc:p.exit(1,str(exc)+'\n')

if __name__=='__main__':main()
