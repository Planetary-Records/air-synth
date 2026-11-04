"""80-digit independent check of actual binary64 weighted tail certificates."""
import decimal,hashlib,json,math,pathlib,sys
decimal.getcontext().prec=80
D=decimal.Decimal
def exact(x):return D.from_float(float(x))
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest().upper()
def require(v,msg):
    if not v:raise AssertionError(msg)
def fields(line):return dict(p.split('=',1) for p in line.split()[1:])
def fixtures():
    for rate in [8000,44100,48000,96000,192000,384000,768000]:
        for shape in range(5):
            for freq in [20,80,320,1280,5120,20000]:
                for gain in ([-18,-6,0,6,18] if shape<3 else [0]):
                    qs=[.25,.5,1,4,12] if shape==0 else [.25,.5,1,4] if shape<3 else [1]
                    if shape==0:
                        critical=.5/10**(gain/40)
                        qs=sorted(set(qs+[critical*s for s in [1-1e-7,1-1e-12,1,1+1e-12,1+1e-7] if .25<=critical*s<=12]))
                    for q in qs:
                        for slope in range(2 if shape>=3 else 1):yield rate,shape,freq,gain,q,slope
def bounds(values,rho):
    b0,b1,b2,a1,a2=map(exact,values);disc=a1*a1-4*a2
    if disc<0:p1=(-a1/2,(-disc).sqrt()/2);p2=(p1[0],-p1[1])
    else:
        root=disc.sqrt();q=-(a1+(root if a1>=0 else -root))/2;p1=(q,D(0));p2=(a2/q if q else D(0),D(0))
    def norm(p):return (p[0]*p[0]+p[1]*p[1]).sqrt()
    def residual(p,h1,h2):return ((h2-p[0]*h1)**2+(p[1]*h1)**2).sqrt()
    r1,r2=norm(p1),norm(p2);require(max(r1,r2)<rho,'actual pole outside circle')
    h1=b1-a1*b0;h2=b2-a1*h1-a2*b0;one=1-r1/rho;two=1-r2/rho
    divided=abs(b0)+abs(h1)/(rho*one)+residual(p1,h1,h2)/(rho*rho*one*two)
    separation=norm((p1[0]-p2[0],p1[1]-p2[1]));partial=D('Infinity')
    if separation:partial=abs(b0)+residual(p2,h1,h2)/(separation*rho*one)+residual(p1,h1,h2)/(separation*rho*two)
    return min(divided,partial),max(r1,r2)
def main():
    root=pathlib.Path(sys.argv[1]);m=read(root/'manifest.json');n=read(root/'native.json')
    require(n['native_exit']==0 and not n['ci_interrupted'],'native exit')
    require(sha(root/(m['target']+'.exe'))==m['binary_sha256'] and sha(root/'controller.ps1')==m['controller_sha256'],'binary/controller')
    require(sha(root/'native.stdout')==n['stdout_sha256'] and sha(root/'native.stderr')==n['stderr_sha256'],'raw')
    for f in m['sources']:require(sha(root/'source'/f['path'])==f['sha256'],f['path'])
    fixed=read(root.parent/'direct-modal-field-green2/manifest.json');source_map=lambda v:{x['path'].replace('\\','/'):x['sha256'] for x in v['sources']}
    a,b=source_map(m),source_map(fixed)
    for name in ['ModalField.cpp','ModalField.h']:require(a['probes/space-reverb/'+name]==b['probes/space-reverb/'+name],'changed qualified helper')
    grouped={}
    for line in (root/'native.stdout').read_text(encoding='utf-8').splitlines():grouped.setdefault(line.split()[0],[]).append(fields(line))
    cases=list(fixtures());configs=grouped['TAIL_GRID_CONFIG'];rows=grouped['TAIL_GRID_ROW'];sections=grouped['TAIL_GRID_SECTION'];require(len(configs)==len(rows)==len(cases),'configuration coverage')
    expected_sections=0;seen=set();ratios=[];pole_margins=[];coefficients={};circles={}
    for i,(c,r,expected) in enumerate(zip(configs,rows,cases)):
        rate,shape,freq,gain,q,slope=expected
        require(int(c['row'])==int(r['row'])==i and c['prepared']==r['pass']=='1','configuration result')
        require((float(c['rate']),int(c['shape']),float(c['frequency']),float(c['gain']),int(c['slope']))==(rate,shape,freq,gain,slope),'configuration identity')
        require(abs(float(c['q'])/q-1)<=1e-12 and float(c['effective'])==min(freq,.45*rate),'q/effective')
        stages=(0 if gain==0 else 1) if shape<3 else 1+slope;require(int(c['stages'])==stages,'stage count');expected_sections+=2*stages
    require(len(sections)==expected_sections,'section coverage')
    for s in sections:
        row,stage,circle=(int(s[k]) for k in ['row','stage','circle']);key=(row,stage,circle)
        require(key not in seen and row<len(cases) and stage<int(configs[row]['stages']) and circle in [0,1] and s['pass']=='1','section identity');seen.add(key)
        values=[s[k] for k in ['b0','b1','b2','a1','a2']];require(all(math.isfinite(float(v)) for v in values+[s['rho'],s['log_l1'],s['maximum_pole']]),'finite binary64')
        rho=exact(s['rho']);require(0<rho<1,'circle range');ideal,pole=bounds(values,rho);native=exact(s['log_l1']).exp();padded=exact(s['maximum_pole'])
        require(native>=ideal,f'underbound row={row} stage={stage} circle={circle} ratio={native/ideal}')
        require(pole<=padded<rho,'padded root bound');ratios.append(float(native/ideal));pole_margins.append(float(padded-pole))
        pair=(row,stage)
        if pair in coefficients:require(values==coefficients[pair],'circle coefficient mutation')
        coefficients[pair]=values;circles[key]=(rho,padded)
    for (row,stage),values in coefficients.items():
        rate=cases[row][0];slow,padded=circles[row,stage,0];tight,padded2=circles[row,stage,1];require(padded==padded2,'circle pole mutation')
        require(abs(float(slow)-math.exp(-math.log(1000)/(rate*46)))<=1e-14,'46s circle')
        pole_time=-math.log(1000)/(rate*math.log(float(padded))) if padded else 0
        require(abs(float(tight)-math.exp(-math.log(1000)/(rate*max(.115,1.15*pole_time))))<=1e-14,'tight circle')
    footer=grouped['TAIL_GRID_FOOTER'];require(len(footer)==1 and footer[0]=={'rows':str(len(cases)),'sections':str(expected_sections),'failed':'0','complete':'1'},'complete footer')
    print(json.dumps({'status':'PASS_SCOPED_80_DIGIT_TAIL_GRID','configurations':len(cases),'section_circle_certificates':len(sections),'decimal_precision':80,'minimum_native_to_ideal_bound_ratio':min(ratios),'maximum_native_to_ideal_bound_ratio':max(ratios),'minimum_padded_pole_margin':min(pole_margins),'manifest_sha256':sha(root/'manifest.json'),'native_record_sha256':sha(root/'native.json'),'scope':'Declared supported post-EQ grid numerical conservatism only; no live/host tail, CPU or product acceptance'},indent=2))
if __name__=='__main__':main()
