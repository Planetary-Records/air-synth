"""Audit diagnostic component-stage measurements; never grants capacity acceptance."""
import hashlib,json,pathlib,sys

STAGES=('peak','final_capture','down','wet_excite','room','mix_rooms','post','up','final_finish','tail_report')

def require(value,message):
    if not value:raise AssertionError(message)

def fields(line):
    tokens=line.split();values=dict(token.split('=',1) for token in tokens[1:])
    require(len(values)==len(tokens)-1,'duplicate field');return tokens[0],values

def parse(lines):
    cases={};samples={};footers=[]
    for line in lines:
        tag,f=fields(line)
        if tag=='MODAL_STAGE_COMPLETE':footers.append(f);continue
        require(tag in ('MODAL_STAGE_CASE','MODAL_STAGE_SAMPLE'),'unknown record')
        c=int(f['case']);require(c in range(4),'case')
        if tag=='MODAL_STAGE_CASE':
            require(c not in cases and f==dict(case=str(c),factor=str(c//2+1),host=str(512 if c%2 else 64),rate='96000',modes='128',slots='8',warm='64',measured='512',valid='1',finite='1',live_adoptions='0',capacity_acceptance='0'),'case metadata')
            cases[c]=f
        else:
            require(set(f)=={'case','parent','stage','ns'},'sample fields')
            parent=int(f['parent']);stage=f['stage'];ns=int(f['ns']);key=(c,parent,stage)
            require(parent in range(512) and stage in STAGES and ns>=0 and key not in samples,'sample identity/value')
            if c<2 and stage in ('up','down'):require(ns==0,'Normal must bypass conversion')
            samples[key]=ns
    require(len(cases)==4 and len(samples)==20480,'complete coverage')
    require(footers==[dict(cases='4',samples='20480',valid='1',caller='ordinary_unpaced',live_adoptions='0',capacity_acceptance='0')],'complete footer')
    rows=[]
    for c in range(4):
        costs=[]
        for stage in STAGES:
            values=sorted(samples[c,n,stage] for n in range(512))
            costs.append(dict(stage=stage,mean_ns=sum(values)/512,p99_ns=values[(99*512+99)//100-1],maximum_ns=values[-1]))
        total=sum(x['mean_ns'] for x in costs)
        for x in costs:x['fraction_of_instrumented_stage_means']=x['mean_ns']/total
        rows.append(dict(case=c,factor=c//2+1,host_frames=512 if c%2 else 64,stage_costs=costs))
    return rows

def audit(root):
    read=lambda p:json.loads(p.read_text(encoding='utf-8-sig'))
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest().upper()
    m=read(root/'manifest.json');n=read(root/'native.json')
    require(m['target']=='RackSynthSpaceReverbModalStageCostProbe' and n['native_exit']==0 and not n['ci_interrupted'] and m['native_resource_lease']=='exclusive','actual native/lease')
    for path,digest in [(m['target']+'.exe',m['binary_sha256']),('controller.ps1',m['controller_sha256']),('native.stdout',n['stdout_sha256']),('native.stderr',n['stderr_sha256'])]:require(sha(root/path)==digest,path)
    require((root/'native.stderr').stat().st_size==0,'native stderr')
    for f in m['sources']:require(sha(root/'source'/f['path'])==f['sha256'],f['path'])
    return dict(status='PASS_SCOPED_MODAL_STAGE_COST',rows=parse((root/'native.stdout').read_text(encoding='utf-8').splitlines()),raw_samples=20480,binary_sha256=m['binary_sha256'],manifest_sha256=sha(root/'manifest.json'),native_receipt_sha256=sha(root/'native.json'),scope='Instrumented static components, ordinary unpaced caller. No live adoption/fade/worker/service-admission workload; timer overhead and cache/order effects retained. No full callback/capacity/heap/product acceptance.')

if __name__=='__main__':print(json.dumps(audit(pathlib.Path(sys.argv[1])),indent=2))
