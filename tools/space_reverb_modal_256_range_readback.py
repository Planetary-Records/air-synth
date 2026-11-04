"""Independent literal-mode and retained-state readback; no CPU acceptance."""
import argparse,bisect,cmath,functools,hashlib,json,math,re,struct
from pathlib import Path
RATES=(8000,44100,48000,96000,192000,384000,768000)
SIZES=(0,.5,1)
LN1000=math.log(1000)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest().upper()
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def fields(line):return dict(re.findall(r'(\S+)=(\S+)',line))
def curve(t=2,b=1):return [t,b,[(False,False,0,1000.,1.,1.) for _ in range(6)]]
def band(shape,f,m,q):return (True,False,shape,f,m,q)
def fixtures():
 out=[]
 for t in (.1,2,20):
  for brightness in (0,.5,1):out.append(curve(t,brightness))
 for m in (.25,4):
  for f in (125,1000,8000):
   for q in (.25,1,4):c=curve();c[2][0]=band(0,f,m,q);out.append(c)
 for shape in (1,2):
  for f in (250,4000):
   for m in (.25,4):c=curve();c[2][0]=band(shape,f,m,1);out.append(c)
 c=curve();c[2]=[band(0,125*2**i,4 if i%2 else .25,4) for i in range(6)];out.append(c)
 c=curve();c[2]=[band(0,1000,4,4)]*6;out.append(c)
 for f in (20,20000):
  for m in (.25,4):
   for q in (.25,1,4):c=curve();c[2][0]=band(0,f,m,q);out.append(c)
 for shape in (1,2):
  for f in (20,20000):
   for m in (.25,4):
    for q in (.25,4):c=curve();c[2][0]=band(shape,f,m,q);out.append(c)
 for f in (20,20000):
  for m in (.25,4):c=curve();c[2]=[band(0,f,m,4)]*6;out.append(c)
 c=curve();c[2]=[band(0,f,4 if i%2 else .25,4) for i,f in enumerate((20,80,320,1280,5120,20000))];out.append(c)
 assert len(out)==70;return out
CURVES=fixtures()
def target(c,f,rate):
 f=min(f,.45*min(rate,384000));v=math.log(c[0])+(1-c[1])*math.log(.05)*f*f/(f*f+16000000)
 for active,bypassed,shape,center,m,q in c[2]:
  if not active or bypassed:continue
  ratio=f/min(center,.45*min(rate,384000))
  w=1/(1+(q*(ratio-1/ratio))**2) if shape==0 else 1/(1+ratio**(2*q))
  if shape==2:w=1-w
  v+=math.log(m)*w
 return max(.1,min(40,math.exp(v)))
def unit(x):
 x&=0xffffffff;x^=x>>16;x=x*0x7feb352d&0xffffffff;x^=x>>15;x=x*0x846ca68b&0xffffffff;x^=x>>16
 return (x&0xffffff)/16777216
def anchors(c,rate):return sorted({20,min(20000,.45*rate),*(min(x,20000,.45*rate) for x in (125,250,500,1000,2000,4000,8000)),*(min(b[3],20000,.45*rate) for b in c[2] if b[0] and not b[1])})
def geometry(c,rate,size,count):
 upper=min(20000,.45*rate);span=math.log((upper+228.83)/248.83)
 out=[248.83*math.exp(((k//8*4+k%4)+(.75 if k//4%2 else .25)+.2*(2*size-1)*(2*unit(k+0x9e3779b9)-1))/(count/2)*span)-228.83 for k in range(count)]
 for branch in (0,1):
  ids=[k for k in range(count) if k//4%2==branch];base=[out[k] for k in ids];claimed=set()
  for anchor in anchors(c,rate):
   at=bisect.bisect_left(base,anchor);lo,hi=at-1,at
   while lo>=0 and ids[lo] in claimed:lo-=1
   while hi<len(ids) and ids[hi] in claimed:hi+=1
   choices=([ids[lo]] if lo>=0 else [])+([ids[hi]] if hi<len(ids) else [])
   selected=min(choices,key=lambda k:(abs(out[k]-anchor),k));out[selected]=anchor;claimed.add(selected)
 return out
def isprime(n):return n>=2 and all(n%d for d in range(2,math.isqrt(n)+1))
@functools.lru_cache(None)
def legacy_sum(rate,size):
 total=0
 for s in (.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719):
  n=math.floor(s*(.5+1.5*size)*rate+.5)
  while not isprime(n):n+=1
  total+=n/rate
 return total
@functools.lru_cache(None)
def region_specs(row):
 c=CURVES[row//21];rate=RATES[row%21//3];size=SIZES[row%3];upper=min(20000,.45*rate);out=[]
 for center in anchors(c,rate):
  half=max(2/legacy_sum(rate,size),center/24);low=max(20,center-half);high=min(upper,center+half)
  minimum=min(target(c,low+(high-low)*j/128,rate) for j in range(129));out.append((center,low,high,-2*LN1000/minimum))
 return out
def recurrence(k,rc,rs,br,bi):
 group=k//4;weights=((1,0),(0,1),(.7071067811865475244,.7071067811865475244),(.7071067811865475244,-.7071067811865475244))[group%4];a=b=0.
 for n in range(64):
  left=.5 if n==0 else ((n*37)%127-63)/32768;right=.25 if n==0 else ((n*53)%127-63)/65536;x=left*weights[0]+right*weights[1]
  a,b=(rc*a-rs*b)+br*x,(rs*a+rc*b)+bi*x
 return a,b
def near(a,b,absolute=1e-11):return math.isclose(a,b,rel_tol=1e-11,abs_tol=absolute)
def audit(evidence,count):
 m=read(evidence/'manifest.json');r=read(evidence/'native.json');side=read(evidence/'sidecar.json')
 assert sha(Path(side['controller_snapshot']))==side['controller_sha256']
 assert sha(Path(side['reader_snapshot']))==side['reader_sha256']==sha(Path(__file__))
 assert m['target']=='RackSynthSpaceReverbDirectModalRangeProbe'
 assert r['native_exit']==0 and not r['ci_interrupted']
 assert sha(evidence/(m['target']+'.exe'))==m['binary_sha256'] and sha(evidence/'controller.ps1')==m['controller_sha256']
 for item in m['sources']:assert sha(evidence/'source'/item['path'].replace('\\','/'))==item['sha256']
 for stream in ('stdout','stderr'):assert sha(evidence/('native.'+stream))==r[stream+'_sha256']
 raw=evidence/'modes.bin';assert sha(raw)==side['sha256'] and raw.stat().st_size==side['bytes']==24+1470*(40+64*count)
 assert m['arguments'][:2]==['--range',str(count)] and Path(m['arguments'][2].strip('"')).resolve()==raw.resolve()
 names={item['path'].replace('\\','/') for item in m['sources']}
 assert {'probes/space-reverb/'+n for n in ('ModalTank.h','ModalTank.cpp','ModalKernel.h','ModalKernel.cpp','ModalAvx2.cpp','DirectModalCases.h','SpaceReverbDirectModalRangeProbe.cpp','CMakeLists.txt')}<=names
 assert {'src/dsp/SpaceReverbModel.h','src/dsp/SpaceReverbModel.cpp','src/dsp/space/CMakeLists.txt','CMakeLists.txt'}<=names
 lines=(evidence/'native.stdout').read_text(encoding='utf-8-sig').splitlines();rows={};regions={};settings={};bands={};footers=[]
 for line in lines:
  if line.startswith('MODAL_RANGE_ROW '):f=fields(line);key=int(f['row']);assert key not in rows;rows[key]=f
  elif line.startswith('MODAL_RANGE_REGION '):f=fields(line);regions.setdefault(int(f['row']),[]).append(f)
  elif line.startswith('MODAL_RANGE_SETTINGS '):f=fields(line);key=int(f['curve']);assert key not in settings;settings[key]=f
  elif line.startswith('MODAL_RANGE_BAND '):f=fields(line);key=(int(f['curve']),int(f['band']));assert key not in bands;bands[key]=f
  elif line.startswith('MODAL_RANGE_FOOTER '):footers.append(fields(line))
 assert list(rows)==list(range(1470)) and len(settings)==70 and len(bands)==420 and len(footers)==1
 for index,c in enumerate(CURVES):
  assert float(settings[index]['decay'])==c[0] and float(settings[index]['brightness'])==c[1]
  for k,b in enumerate(c[2]):
   f=bands[index,k];actual=(bool(int(f['active'])),bool(int(f['bypass'])),int(f['shape']),float(f['frequency']),float(f['multiplier']),float(f['q']));assert actual==b,(index,k)
 maximum_error=maximum_residual=maximum_weighted=0.;maximum_bytes=total_regions=0
 with raw.open('rb') as stream:
  assert struct.unpack('<QIIII',stream.read(24))==(0x31454c4f504d5244,1,count,0,1470)
  for row in range(1470):
   saved=rows[row];rate=RATES[row%21//3];size=SIZES[row%3];c=CURVES[row//21];upper=min(20000,.45*rate)
   index,n,fs,s,pickup,weighted=struct.unpack('<IIdddd',stream.read(40));assert (index,n,fs,s)==(row,count,rate,size)
   reference=math.sqrt(-math.expm1(-LN1000/48000));loss=-math.expm1(-LN1000/(2*rate));reference_loss=-math.expm1(-LN1000/(2*48000))
   assert near(pickup,math.sqrt(4/count)*reference*(loss/reference_loss),1e-15)
   payload=stream.read(64*count);assert len(payload)==64*count;expected_geometry=geometry(c,rate,size,count)
   frequencies=[];reals=[];errors=[];residuals=[];max_radius=0
   selected={0,4,8,12,count//3,count//2,count-5,count-1}
   for k,(f,rc,rs,br,bi,t,a,b) in enumerate(struct.iter_unpack('<8d',payload)):
    assert all(math.isfinite(v) for v in (f,rc,rs,br,bi,t,a,b));assert abs(f-expected_geometry[k])<=1e-8
    sign=-1. if unit(k+0x3c6ef372)<.5 else 1.;imaginary=((k//4//4)+(k//4%4)//2)%2!=0;assert (br,bi)==((0.,sign) if imaginary else (sign,0.))
    radius=math.hypot(rc,rs);assert 0<radius<1;max_radius=max(max_radius,radius);hz=math.atan2(rs,rc)*rate/(2*math.pi);assert abs(hz-f)<=1e-8
    real=rate*math.log(radius);actual=-LN1000/real;wanted=target(c,max(20,min(upper,hz)),rate);error=abs(actual/wanted-1)
    assert near(t,target(c,f,rate)) and error<=1e-6
    residual=max(abs(z*z-2*rc*z+(rc*rc+rs*rs)) for z in (complex(rc,rs),complex(rc,-rs)));assert residual<=1e-9
    frequencies.append(hz);reals.append(real);errors.append(error);residuals.append(residual)
    if k in selected:
     aa,bb=recurrence(k,rc,rs,br,bi);assert near(a,aa,5e-13) and near(b,bb,5e-13),(row,k,a,aa,b,bb)
   calculated=max_radius/math.exp(-LN1000/(rate*46));assert 0<calculated<1 and near(calculated,weighted,1e-12)
   assert int(saved['row'])==row and int(saved['curve'])==row//21 and float(saved['rate'])==rate and float(saved['size'])==size
   assert int(saved['modes'])==count and int(saved['factors'])==2*count and saved['bit_audio']==saved['bit_state']==saved['pass']=='1'
   assert saved['allocations']==saved['frees']=='0' and saved['backend'] in ('sse2','avx2','scalar')
   assert 0<int(saved['bytes'])<=16*1024*1024 and near(float(saved['weighted']),calculated,1e-12)
   assert near(float(saved['error']),max(errors),5e-9) and near(float(saved['residual']),max(residuals),1e-12)
   specs=region_specs(row);assert len(regions[row])==len(specs)
   for saved_region,(center,low,high,left) in zip(regions[row],specs):
    assert all(near(float(saved_region[key]),v) for key,v in zip(('center','low','high','left'),(center,low,high,left)))
    admitted=[k for k in range(count) if low-1e-8<=frequencies[k]<=high+1e-8 and left<=reals[k]<=1e-6];assert admitted
    assert int(saved_region['positive_factors'])==len(admitted) and saved_region['pass']=='1'
    assert max(errors[k] for k in admitted)<=.15 and max(residuals[k] for k in admitted)<=1e-9
    assert near(float(saved_region['error']),max(errors[k] for k in admitted),5e-9) and near(float(saved_region['residual']),max(residuals[k] for k in admitted),1e-12)
   total_regions+=len(specs);maximum_error=max(maximum_error,max(errors));maximum_residual=max(maximum_residual,max(residuals));maximum_weighted=max(maximum_weighted,calculated);maximum_bytes=max(maximum_bytes,int(saved['bytes']))
  assert stream.read(1)==b''
 f=footers[0];assert int(f['count'])==count and f['rows']=='1470' and f['failed']=='0' and f['complete']=='1'
 assert int(f['modes'])==1470*count and int(f['factors'])==2940*count and int(f['regions'])==total_regions and int(f['sidecar_bytes'])==side['bytes']
 return dict(count=count,rows=1470,conjugate_factors=2940*count,regions=total_regions,maximum_time_error=maximum_error,maximum_quadratic_residual=maximum_residual,maximum_weighted_radius=maximum_weighted,maximum_candidate_only_bytes=maximum_bytes,selected_states_independently_recomputed=1470*8,manifest_sha256=sha(evidence/'manifest.json'),native_record_sha256=sha(evidence/'native.json'),sidecar_sha256=side['sha256'],sidecar_bytes=side['bytes'])
def main():
 parser=argparse.ArgumentParser();parser.add_argument('evidence256',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
 result=audit(args.evidence256,256)
 report=dict(status='PASS_SCOPED_COMPLETE_DIRECT_MODAL_256_RANGE',results=[result],rows=1470,conjugate_factors=752640,scope='256 literal poles and64-frame exactness only; stationary raw audio, density/listening, service and CPU remain separate',routing='Parent only; no subagents')
 with args.output.open('x',encoding='utf-8') as f:json.dump(report,f,indent=2);f.write('\n')
 print(json.dumps(dict(status=report['status'],rows=1470,factors=752640)))
if __name__=='__main__':main()
