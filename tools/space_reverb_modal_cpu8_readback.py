"""Recompute original processing-budget gate from ordered CPU8 records."""
import hashlib,json,math,pathlib,struct,sys
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest().upper()
def require(ok,message):
    if not ok:raise AssertionError(message)
def fields(line):return dict(x.split('=',1) for x in line.split()[1:])
def close(a,b):return math.isfinite(a) and math.isfinite(b) and abs(a-b)<=1e-11*max(abs(a),abs(b),1e-10)
def stats(v):
    if not v:return (0,0,0)
    return (sum(v)/len(v),sorted(v)[math.ceil(.99*len(v))-1],max(v))
def main():
    root=pathlib.Path(sys.argv[1]);m=read(root/'manifest.json')
    require(m['condition']=='direct-modal1024-normal-unpaced8' and m['deadline']=='processing_entry_to_exit','condition')
    require(m['warm_seconds']==10 and m['measure_seconds']==20 and m['kernel_mean_p99_fraction']==.5 and m['copy_p99_fraction']==.1,'original budgets')
    for file,key in [('ModalCpuFrozen.exe','binary_sha256'),('controller.ps1','controller_sha256'),('reader.py','reader_sha256')]:require(sha(root/file)==m[key],file)
    for s in m['sources']:require(sha(root/'source'/s['path'])==s['sha256'],s['path'])
    attempts=[];complete_rows=[];all_misses=[];total_samples=0
    for p in sorted(root.glob('row-*-attempt-*.json')):
        a=read(p);row=a['row'];rate=[44100,48000,96000][row//4];block=[64,512][row//2%2];period=block/rate;prefix=p.stem
        for raw in a['records']:require(sha(root/raw['file'])==raw['sha256'] and (root/raw['file']).stat().st_size==raw['bytes'],raw['file'])
        lines=(root/(prefix+'.stdout')).read_text(encoding='utf-8-sig').splitlines();results=[fields(x) for x in lines if x.startswith('MODAL_HANDOFF_CPU_RESULT ')]
        if not results:
            require(a['exit']==3 and any(x.startswith('MODAL_HANDOFF_CPU_ADMISSION_BLOCKED ') for x in lines),'missing result')
            attempts.append({'row':row,'attempt':a['attempt'],'status':'ADMISSION_BLOCKED','samples':0});continue
        require(len(results)==1,'result count');r=results[0];require(int(r['row'])==row and float(r['rate'])==rate and int(r['block'])==block and r['domain']=='directModal','settings')
        records=list(struct.iter_unpack('<ddQ',(root/(prefix+'.timings.bin')).read_bytes()));cycles=[x[0] for x in struct.iter_unpack('<Q',(root/(prefix+'.timings.bin.cycles.bin')).read_bytes())]
        require(len(records)==len(cycles)==int(r['samples']),'raw counts');times=[v[0] for v in records];copies=[v[1] for v in records if v[2]]
        require(all(math.isfinite(t) and t>=0 and math.isfinite(c) and 0<=c<=t for t,c,b in records),'ordered timing values')
        require(all(c==0 for t,c,b in records if not b),'no copy marker/time mismatch')
        ts,cs=stats(times),stats(copies)
        for k,v in zip(['mean_seconds','p99_seconds','max_seconds'],ts):require(close(float(r[k]),v),k)
        for k,v in zip(['copy_mean_seconds','copy_p99_seconds','copy_max_seconds'],cs):require(close(float(r[k]),v),k)
        require(int(r['copy_events'])==len(copies) and close(float(r['period_seconds']),period) and close(float(r['limit_seconds']),.5*period) and close(float(r['copy_limit_seconds']),.1*period),'copy/budgets')
        copy_indices=[j for j,v in enumerate(records) if v[2]];spacing=math.ceil((math.ceil(.005*rate)+1)/block)
        require(all(b-a==spacing for a,b in zip(copy_indices,copy_indices[1:])),'copy cadence')
        require(len({v[2] for v in records if v[2]})<=1 and all(v[2]>=2*1024*8 for v in records if v[2]),'copy byte bound')
        misses=[{'row':row,'attempt':a['attempt'],'ordinal':j,'kernel_seconds':t,'period_seconds':period,'copy_seconds':c,'copy_upper_bytes':b,'thread_cycles':cycles[j]} for j,(t,c,b) in enumerate(records) if t>period]
        require(int(r['misses'])==len(misses),'miss count');printed=[fields(x) for x in lines if x.startswith('MODAL_HANDOFF_DEADLINE_MISS ')]
        require([int(v['ordinal']) for v in printed]==[v['ordinal'] for v in misses],'miss order')
        for x,y in zip(printed,misses):require(close(float(x['seconds']),y['kernel_seconds']) and close(float(x['kernel_seconds']),y['kernel_seconds']),'miss value')
        require('MODAL_HANDOFF_ALLOCATION_DOMAIN render_thread=1' in lines and 'MODAL_HANDOFF_WORKLOAD order=tank_major scratch_bytes=147456' in lines,'workload')
        require(any(x.startswith('MODAL_HANDOFF_INVENTORY_MONITOR mode=worker ') and 'cadence_ms=100 freshness_ms=250' in x for x in lines),'monitor')
        require(f'MODAL_HANDOFF_CYCLE_DIAGNOSTIC enabled=1 valid=1 samples={len(records)} format=uint64_native_little_endian outside_block_timer=1' in lines,'cycles declaration')
        require(not any(x.startswith('MODAL_HANDOFF_PERIODIC_CALLER ') for x in lines),'unpaced condition')
        complete=r['interrupted']=='0';passed=complete and float(r['warm_seconds'])>=10 and float(r['measure_seconds'])>=20 and bool(times) and bool(copies) and ts[0]<=.5*period and ts[1]<=.5*period and cs[1]<=.1*period and not misses and r['allocations']==r['frees']=='0' and r['adopted']=='1' and r['overflow']=='0' and math.isfinite(float(r['checksum'])) and 0<float(r['peak'])<4
        require((r['pass']=='1')==passed,'reported pass')
        if complete:
            require(a['exit']==(0 if passed else 1) and f'MODAL_HANDOFF_CPU_FOOTER row={row} complete=1 acceptance=0' in lines,'actual complete exit')
            c=read(root/f'row-{row:02d}.complete.json');require(c['row']==row and c['attempt']==a['attempt'] and c['exit']==a['exit'] and c['valid'],'controller completion');complete_rows.append(row)
        else:require(a['exit']==3,'interrupted exit')
        total_samples+=len(records);all_misses.extend(misses)
        attempts.append({'row':row,'attempt':a['attempt'],'complete':complete,'pass':passed,'samples':len(records),'mean_period_fraction':ts[0]/period,'p99_period_fraction':ts[1]/period,'copy_p99_period_fraction':cs[1]/period,'misses':len(misses),'allocations':int(r['allocations']),'frees':int(r['frees']),'candidate_only_bytes':int(r['prepared_bytes'])})
    passed=sorted(complete_rows)==list(range(12)) and all(a.get('pass',True) for a in attempts) and not all_misses
    print(json.dumps({'status':'PASS_SCOPED_CORE' if passed else 'NOT_PASS','condition':m['condition'],'scope':'stationary modal copied-history processing only; no Character/outer field/background preparation/Higher/host/product acceptance','complete_rows':complete_rows,'measured_samples':total_samples,'attempts':attempts,'misses':all_misses,'manifest_sha256':sha(root/'manifest.json')},indent=2))
if __name__=='__main__':main()
