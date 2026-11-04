"""Read every paired converter trial; component direction gives no rack capacity credit."""
import hashlib
import json
import pathlib
import sys


def require(value, message):
    if not value:
        raise AssertionError(message)


def fields(line):
    tokens=line.split()
    values=dict(token.split('=',1) for token in tokens[1:])
    require(len(values)==len(tokens)-1,'duplicate field')
    return tokens[0],values


def parse(lines):
    configurations=[(2,128,128,0),(2,1024,512,0),(2,128,17,0),(2,128,128,1),(1,64,64,0)]
    pairs={};samples={};footers=[]
    for line in lines:
        tag,f=fields(line)
        if tag=='CONVERTER_AVX4_COST_COMPLETE':
            footers.append(f);continue
        require(tag in ('CONVERTER_AVX4_COST_PAIR','CONVERTER_AVX4_COST_SAMPLE'),'unknown record')
        c,backend,order=(int(f[k]) for k in ('case','requested','order'))
        require(c in range(5) and backend in (4,) and order in (0,1),'pair identity')
        key=c,backend,order
        if tag=='CONVERTER_AVX4_COST_PAIR':
            factor,frames,packet,cold=configurations[c]
            expected=dict(case=str(c),requested=str(backend),actual=str(backend),order=str(order),
                factor=str(factor),physical=str(frames),packet=str(packet),cold=str(cold),slots='8',warm='64',measured='512',
                exact='1',finite='1',valid='1',reference_backend='mirrored_sse2',capacity_acceptance='0')
            require(f==expected and key not in pairs,'actual configuration/exactness/duplicate pair')
            pairs[key]=f
        else:
            require(set(f)=={'case','requested','order','parent','position','candidate','ns'},'sample fields')
            parent,position,candidate,ns=(int(f[k]) for k in ('parent','position','candidate','ns'))
            require(parent in range(512) and position in (0,1) and candidate==position^order and ns>0,'sample order/value')
            sample_key=key+(parent,candidate)
            require(sample_key not in samples,'duplicate sample')
            samples[sample_key]=ns
    require(len(pairs)==10 and len(samples)==10240,'complete pair/sample coverage')
    require(footers==[dict(pairs='10',samples='10240',exact_and_contracts='1',caller='ordinary_unpaced',workers='none',capacity_acceptance='0')],'complete footer')
    result=[]
    for key in sorted(pairs):
        original=sorted(samples[key+(parent,0)] for parent in range(512))
        candidate=sorted(samples[key+(parent,1)] for parent in range(512))
        # Same nearest-rank p99 as the full gate; retain every pair and maximum.
        rank=(99*512+99)//100-1
        a=dict(mean_ns=sum(original)/512,p99_ns=original[rank],maximum_ns=original[-1])
        b=dict(mean_ns=sum(candidate)/512,p99_ns=candidate[rank],maximum_ns=candidate[-1])
        result.append(dict(case=key[0],requested=key[1],actual=int(pairs[key]['actual']),order=key[2],samples_per_side=512,
            original=a,candidate=b,candidate_to_original_mean=b['mean_ns']/a['mean_ns'],candidate_to_original_p99=b['p99_ns']/a['p99_ns']))
    return result


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def audit(root):
    m=read(root/'manifest.json');n=read(root/'native.json')
    require(m['target']=='RackSynthSpaceReverbModalConverterAvx4CostProbe' and m['arguments']==[] and m['native_resource_lease']=='exclusive','native identity')
    require(n['native_exit']==0 and not n['ci_interrupted'],'actual native exit')
    require(sha(root/(m['target']+'.exe'))==m['binary_sha256'] and sha(root/'controller.ps1')==m['controller_sha256'],'binary/controller')
    require(sha(root/'native.stdout')==n['stdout_sha256'] and sha(root/'native.stderr')==n['stderr_sha256'],'raw stream hashes')
    require((root/'native.stderr').stat().st_size==0,'native stderr')
    for binding in m['sources']:
        require(sha(root/'source'/binding['path'])==binding['sha256'],binding['path'])
    source=root/'source/probes/space-reverb'
    frozen=root.parent/'avx4-cpu23-source1'
    for extension in ('h','cpp'):
        ref=(source/f'ModalConverterSseReference.{extension}').read_bytes()
        ref=ref.replace(b'namespace airack::space::probe::converter_sse_reference',b'namespace airack::space::probe')
        if extension=='cpp':ref=ref.replace(b'"ModalConverterSseReference.h"',b'"ModalConverter.h"')
        require(ref==(frozen/f'ModalConverter.{extension}').read_bytes(),'original source byte inverse')
    cmake=(source/'CMakeLists.txt').read_text(encoding='utf-8-sig')
    require('target_compile_options(RackSynthSpaceReverbModalConverterAvx4CostProbe PRIVATE /O2 /fp:strict)' in cmake,'declared strict probe compile options')
    rows=parse((root/'native.stdout').read_text(encoding='utf-8').splitlines())
    return dict(status='PASS_SCOPED_PAIRED_CONVERTER_AVX4_COST',pairs=10,raw_samples=10240,source_bindings=len(m['sources']),
        binary_sha256=m['binary_sha256'],manifest_sha256=sha(root/'manifest.json'),native_receipt_sha256=sha(root/'native.json'),
        original_reference='Exact namespace/include-only byte inverse of retired CPU23 SSE2 converter',rows=rows,
        scope='Directional component cost only; all orders/trials retained. Ordinary unpaced caller/no coefficient workers. No full-rack capacity, Release heap, product or host acceptance.')


if __name__=='__main__':
    print(json.dumps(audit(pathlib.Path(sys.argv[1])),indent=2))
