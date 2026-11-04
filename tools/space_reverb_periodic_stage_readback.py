"""Direction-only attribution of actual dynamic periodic service work; never CPU acceptance."""
import argparse,csv,hashlib,io,json,math
from pathlib import Path
NAMES=('capture','down','excite','room','room_fade','mix','post','up','final','publish')
def _require(value,text):
    if not value:raise ValueError(text)
def _summary(values):
    values=sorted(values);n=len(values)
    return {'count':n,'mean_us':sum(values)/n/1000 if n else 0,'p50_us':values[n//2]/1000 if n else 0,'p99_us':values[(99*n+99)//100-1]/1000 if n else 0,'maximum_us':max(values,default=0)/1000}
def decode(text):
    lines=text.splitlines();_require(len(lines)>=3,'missing rows');parts=lines[0].split(',');_require(parts[0]=='MODAL_PERIODIC_STAGE_V1','header version');meta=dict(x.split('=',1) for x in parts[1:])
    for key,value in {'row':'10','rate':'96000','host':'64','factor':'2','modes':'128','slots':'8','warm':'15000','expected':'4096','valid':'1','interrupted':'0','reason':'none','closed':'1','capacity_acceptance':'0'}.items():_require(meta.get(key)==value,'metadata '+key)
    frequency=int(meta['frequency']);_require(0<frequency<=10**9 and meta['paced'] in ('0','1'),'clock/caller');paced=meta['paced']=='1'
    if paced:_require(meta['opened']=='1' and meta['priority_before']==meta['priority_after'],'caller restoration')
    expected=('ordinal','release','start','begin','finish','cycles','maximum_rooms','room_copy','post_admission','cold_install','valid','allocations','frees')+tuple(n+'_ns' for n in NAMES)+tuple(n+'_calls' for n in NAMES)
    reader=csv.DictReader(io.StringIO('\n'.join(lines[1:])));_require(tuple(reader.fieldnames or ())==expected,'CSV columns');rows=[];previous=0;anchor=None
    for index,raw in enumerate(reader):
        _require(None not in raw and all(raw[k] is not None for k in expected),'row width');r={k:int(raw[k]) for k in expected};_require(all(v>=0 for v in r.values()),'negative value')
        _require(r['ordinal']==index and r['release']<=r['start']<=r['begin']<=r['finish'] and r['start']>=previous and r['cycles']>0,'chronology');previous=r['finish']
        if paced:
            if anchor is None:anchor=r['release']-frequency*64//96000
            _require(r['release']==anchor+(index+1)*frequency*64//96000,'rational release')
        _require(r['valid']==1 and r['allocations']==r['frees']==r['cold_install']==0,'callback contract')
        _require(r['maximum_rooms'] in (8,9) and r['room_copy'] in (0,1) and r['post_admission'] in (0,1) and r['room_copy']+r['post_admission']<=1,'admission flags')
        _require(not r['room_copy'] or r['maximum_rooms']==9,'copy flag');_require(not r['post_admission'] or r['maximum_rooms']==8,'post flag')
        for name in NAMES:_require(r[name+'_calls']==(int(r['maximum_rooms']==9) if name=='room_fade' else 1 if name=='publish' else 8),'stage call coverage '+name)
        _require(sum(r[name+'_ns'] for name in NAMES)<=(r['finish']-r['start'])*10**9/frequency+2000,'overlapping stage clocks')
        rows.append(r)
    _require(len(rows)==4096,'incomplete run')
    def group(items):
        totals=[(r['finish']-r['start'])*10**9/frequency for r in items];mean_total=sum(totals)/len(totals) if totals else 0
        return {'samples':len(items),'total':_summary(totals),'begin':_summary([(r['begin']-r['start'])*10**9/frequency for r in items]),'render_end':_summary([(r['finish']-r['begin'])*10**9/frequency for r in items]),'stages':{name:{**_summary([r[name+'_ns'] for r in items]),'mean_fraction_of_total':sum(r[name+'_ns'] for r in items)/len(items)/mean_total if items and mean_total else 0} for name in NAMES},'unattributed':_summary([(r['finish']-r['start'])*10**9/frequency-sum(r[name+'_ns'] for name in NAMES) for r in items]),'processing_overruns_observed':sum((r['finish']-r['start'])*96000>frequency*64 for r in items)}
    return {'status':'PASS_DIRECTION_ONLY_PERIODIC_STAGE_ATTRIBUTION','capacity_acceptance':False,'instrumentation_overhead_retained':True,'metadata':meta,'all':group(rows),'eight_rooms':group([r for r in rows if r['maximum_rooms']==8]),'nine_rooms':group([r for r in rows if r['maximum_rooms']==9]),'room_admission':group([r for r in rows if r['room_copy']]),'post_admission':group([r for r in rows if r['post_admission']])}
def main():
    parser=argparse.ArgumentParser();parser.add_argument('csv',type=Path);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();data=args.csv.read_bytes();report=decode(data.decode('utf-8-sig'));report['csv_sha256']=hashlib.sha256(data).hexdigest();args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps({'status':report['status'],'samples':report['all']['samples'],'total':report['all']['total'],'stages':{k:v['mean_us'] for k,v in report['all']['stages'].items()},'capacity_acceptance':False}));return 0
if __name__=='__main__':raise SystemExit(main())
