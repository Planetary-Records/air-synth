"""Independent immutable stationary modal audio readback; no CPU claim."""
import hashlib,json,math,pathlib,struct,sys
def read_json(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest().upper()
def require(ok,message):
    if not ok:raise AssertionError(message)
def fields(line):return dict(part.split('=',1) for part in line.split()[1:])
def close(a,b,tol=1e-12):return math.isfinite(a) and math.isfinite(b) and abs(a-b)<=tol*max(abs(a),abs(b),1e-300)
def fnv(data,h=14695981039346656037):
    for b in data:h=((h^b)*1099511628211)&0xffffffffffffffff
    return h
def main():
    evidence,range_dir=map(pathlib.Path,sys.argv[1:3]);m=read_json(evidence/'manifest.json');count=m['count']
    require(sha(evidence/'AudioFrozen.exe')==m['binary_sha256'],'audio binary')
    require(sha(evidence/'controller.ps1')==m['controller_sha256'] and sha(evidence/'reader.py')==m['reader_sha256'],'frozen tools')
    for f in m['sources']:require(sha(evidence/'source'/f['path'])==f['sha256'],f['path'])
    rm=read_json(range_dir/'manifest.json');side=read_json(range_dir/'sidecar.json');data=(range_dir/'modes.bin').read_bytes()
    require(hashlib.sha256(data).hexdigest().upper()==side['sha256'] and len(data)==side['bytes'],'range binary sidecar')
    rn=read_json(range_dir/'native.json');require(rn['native_exit']==0 and not rn['ci_interrupted'],'range actual exit')
    for name in ['ModalTank.cpp','ModalTank.h','ModalKernel.cpp','ModalKernel.h','ModalAvx2.cpp','DirectModalCases.h','ProbeCases.h']:
        rel='probes\\space-reverb\\'+name
        a=next(f for f in m['sources'] if f['path'].replace('/','\\')==rel)
        b=next(f for f in rm['sources'] if f['path'].replace('/','\\')==rel)
        require(a['sha256']==b['sha256'],'changed qualified core '+name)
    require(struct.unpack_from('<QIIII',data)==(0x31454c4f504d5244,1,count,0,1470),'range layout')
    corners=[0,20,42,62,168,188,1365,1372,1385,1412,1414,1415,1418,1421,1424,1427,1428,1448,1449,1452,1455,1461,1462,1469]
    workloads=[('calibrate',49,112)]+[('row',i,(i//9)*21+(i%9//3+1)*3+i%3) for i in range(333) if i!=49]+[('corner',i,r) for i,r in enumerate(corners)]
    require([(w['mode'],w['ordinal'],w['row']) for w in m['workloads']]==workloads,'workload manifest')
    corpus={};peaks=[];tails=[];energies=[];bindings=[];rates=[8000,44100,48000,96000,192000,384000,768000]
    for mode,i,row in workloads:
        key=f'{mode}-{i:03d}';complete=read_json(evidence/(key+'.complete.json'));attempt=complete['attempt'];raw=evidence/complete['stdout'];meta=read_json(evidence/f'{key}-attempt-{attempt}.json')
        require(complete['exit']==0 and complete['valid'] and meta['exit']==0 and not meta['ci_interrupted'],key+' actual exit')
        require(sha(raw)==complete['stdout_sha256']==meta['stdout_sha256'] and sha(evidence/f'{key}-attempt-{attempt}.stderr')==meta['stderr_sha256'],key+' raw hashes')
        rows={}
        for line in raw.read_text(encoding='utf-8-sig').splitlines():rows.setdefault(line.split()[0],[]).append(fields(line))
        for tag in ['MODAL_AUDIO_PREP','MODAL_AUDIO_RAW','MODAL_AUDIO_FOOTER']:require(len(rows.get(tag,[]))==1,key+' '+tag)
        p,r,f=(rows[tag][0] for tag in ['MODAL_AUDIO_PREP','MODAL_AUDIO_RAW','MODAL_AUDIO_FOOTER'])
        for entry in [p,r,f]:require((entry['mode'],int(entry['ordinal']),int(entry['row']))==(mode,i,row),key+' identity')
        require(f['failed']=='0' and f['complete']=='1' and p['prepared']=='1' and int(p['count'])==count,key+' completion')
        off=24+row*(40+count*64);header=data[off:off+40];rr,nn,rate,size,pickup,weighted=struct.unpack('<IIdddd',header)
        require(rr==row and nn==count and float(p['rate'])==rate==rates[row%21//3] and float(p['size'])==size==[0,.5,1][row%3],key+' settings')
        h=fnv(header)
        for k in range(count):h=fnv(data[off+40+k*64:off+40+k*64+48],h)
        require(int(p['bank'])==h,key+' prepared bank binding');bindings.append(h)
        peak,energy,tail,fraction=(float(r[k]) for k in ['peak','energy','tail_energy','tail_fraction'])
        require(all(math.isfinite(v) for v in [peak,energy,tail,fraction]) and 0<peak<=4 and energy>0 and 0<=tail and fraction<=1e-6 and close(fraction,tail/energy),key+' raw stability')
        require(all(r[k]=='1' for k in ['finite','rendered','meters','pass']) and r['allocations']==r['frees']=='0' and int(r['bytes'])<=16*1024*1024,key+' native checks')
        longest=float(r['longest']);require(int(r['frames'])==math.ceil(max(2,1.25*longest+.3)*rate),key+' duration')
        actual_max=max(struct.unpack_from('<d',data,off+40+k*64+40)[0] for k in range(count));require(longest>=actual_max,key+' longest actual mode')
        peaks.append(peak);tails.append(fraction);energies.append(energy)
        if mode!='corner':
            meters=rows.get('MODAL_AUDIO_METER',[]);require(len(meters)==7,key+' meters');ratios=[]
            for meter,freq in zip(meters,[125,250,500,1000,2000,4000,8000]):
                early,late,ratio=(float(meter[k]) for k in ['early','late','ratio']);require(int(meter['frequency'])==freq and early>0 and late>=0 and close(ratio,late/early) and meter['pass']=='1',key+' meter sums');ratios.append(ratio)
                if mode=='calibrate':
                    require(close(float(meter['twice_early']),4*early) and close(float(meter['twice_late']),4*late) and close(float(meter['twice_ratio']),ratio) and float(meter['ratio_error'])<=1e-12,key+' gain invariant')
            corpus[i]=ratios
        if mode=='calibrate':
            require(len(rows.get('MODAL_AUDIO_CALIBRATION',[]))==1,'calibration count');c=rows['MODAL_AUDIO_CALIBRATION'][0]
            require(c['pass']=='1' and close(float(c['energy']),energy) and close(float(c['twice_energy']),4*energy) and float(c['error'])<=1e-12,'calibration energy')
    directions=[]
    for curve in range(9,35):
        for context in range(9):
            ordinal=curve*9+context;band={125:0,250:1,1000:3,4000:5,8000:6}[125 if curve<12 or 18<=curve<21 else 1000 if 12<=curve<15 or 21<=curve<24 else 8000 if curve<27 else 250 if curve in [27,28,31,32] else 4000]
            multiplier=.25 if (curve<18 or curve in [27,29,31,33]) else 4
            relative=corpus[ordinal][band]/corpus[45+context][band]
            require(math.isfinite(relative) and (relative<.5 if multiplier==.25 else relative>2),f'direction curve={curve} context={context} relative={relative}')
            directions.append(relative)
    require(len(corpus)==333 and len(directions)==234,'complete original corpus')
    require(read_json(evidence/'summary.json')['workloads']==357,'controller summary')
    result={'status':'PASS','scope':'stationary rendered audio only; no CPU, modulation, service, density/listening or product acceptance','count':count,'native_workloads':357,'original_corpus':333,'corners':24,'fixed_meter_direction_comparisons':234,'maximum_raw_peak':max(peaks),'maximum_raw_tail_fraction':max(tails),'raw_energy_range':[min(energies),max(energies)],'exact_bank_bindings':len(bindings),'audio_manifest_sha256':sha(evidence/'manifest.json'),'range_sidecar_sha256':side['sha256'],'direction_relative_range':[min(directions),max(directions)]}
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
