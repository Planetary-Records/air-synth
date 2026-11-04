"""Analyse retained ReferencePluginProbe impulses; never load a plugin or alter audio."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decay(audio, rate, frequency=None):
    if frequency is not None:
        audio=sosfilt(butter(4,[frequency/np.sqrt(2),frequency*np.sqrt(2)],btype='bandpass',fs=rate,output='sos'),audio,axis=0)
    energy=np.sum(np.asarray(audio,dtype=np.float64)**2,axis=1)
    remaining=np.cumsum(energy[::-1])[::-1]
    if not len(remaining) or remaining[0]<=0:
        return np.full(len(energy),-300.),None
    db=10*np.log10(np.maximum(remaining/remaining[0],1e-30))
    mask=(db<=-5)&(db>=-25)
    if np.count_nonzero(mask)<64:
        return db,None
    t=np.arange(len(db),dtype=np.float64)/rate
    slope,intercept=np.polyfit(t[mask],db[mask],1)
    prediction=slope*t[mask]+intercept
    residual=np.sum((db[mask]-prediction)**2);total=np.sum((db[mask]-np.mean(db[mask]))**2)
    return db,{'rt60_t20_extrapolation_seconds':float(-60/slope) if slope<0 else None,
               'fit_r_squared':float(1-residual/total) if total else None,
               'fit_start_seconds':float(t[mask][0]),'fit_end_seconds':float(t[mask][-1])}


def self_test():
    rate=48000;t=np.arange(4*rate)/rate
    signal=np.column_stack([np.exp(-np.log(1000)*t/1.5)*np.sin(2*np.pi*1000*t)]*2)
    first,fit=decay(signal,rate);scaled,again=decay(signal*2,rate)
    assert abs(fit['rt60_t20_extrapolation_seconds']-1.5)<.002
    assert np.max(np.abs(first-scaled))<1e-10 and fit==again
    _,band=decay(signal,rate,1000);assert abs(band['rt60_t20_extrapolation_seconds']-1.5)<.01
    assert decay(np.zeros((32,2)),rate)[1] is None
    print('REFERENCE_ANALYSIS_SELF_TEST PASS: decay calibration, amplitude invariance, band fit, silence')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path);parser.add_argument('--output',type=Path)
    parser.add_argument('--self-test',action='store_true');args=parser.parse_args()
    if args.self_test:self_test();return
    if args.root is None or args.output is None:parser.error('--root and --output are required')
    assert not args.output.exists(),'Use a fresh analysis output'
    args.output.mkdir(parents=True)
    inspect=args.root/'reference-inspect2/report.json';inspection=json.loads(inspect.read_text(encoding='utf-8-sig'))
    assert inspection['success'] and inspection['plugin_unchanged']
    records=[];series={}
    for name in ['space','decay-rate','predelay','width','brightness','character','distance','ducking','decay-eq','post-eq']:
        folder=args.root/('reference-render-'+name);report=json.loads((folder/'report.json').read_text(encoding='utf-8-sig'))
        assert report['success'] and report['plugin_unchanged'] and report['plugin_before']['sha256']==inspection['plugin_before']['sha256']
        series[name]=[]
        for row in report['renders']:
            file=folder/Path(row['wav']['path']).name
            assert sha(file)==row['wav']['sha256'] and row['wav_readback_bit_exact']
            rate,audio=wavfile.read(file);assert rate==48000 and audio.dtype==np.float32 and np.isfinite(audio).all()
            assert audio.shape==(row['frames'],2)
            broadband,fit=decay(audio,rate);bands={str(f):decay(audio,rate,f)[1] for f in [125,1000,8000]}
            difference=np.max(np.abs(audio[:,0].astype(np.float64)-audio[:,1]))
            magnitude=np.max(np.abs(audio),axis=1);significant=np.flatnonzero(magnitude>max(float(magnitude.max())*1e-6,1e-12))
            variation=row.get('single_parameter_variation');label='baseline'
            if variation:
                actual=next(p for p in row['actual_settings'] if p['id']==variation['id'])
                label=actual['text']
            result={'case':name,'ordinal':row['ordinal'],'signal':row['signal'],'label':label,
                    'single_parameter_variation':variation,'rate':rate,'frames':len(audio),'wav':row['wav'],
                    'broadband_fit':fit,'octave_band_fits':bands,'maximum_L_minus_R':float(difference),
                    'significant_onset_seconds':float(significant[0]/rate) if len(significant) else None,
                    'peak':row['peak'],'latency_samples':row['latency_samples'],'tail_seconds':row['tail_seconds'],
                    'tail_kind':row['tail_kind'],'tail_finite':row['tail_finite']}
            records.append(result)
            if row['signal']=='mono_left_impulse':series[name].append((result,audio,broadband))
    fig,axes=plt.subplots(2,2,figsize=(12,7.5),layout='constrained')
    for row,audio,db in series['space']:
        if row['single_parameter_variation']:axes[0,0].plot(np.arange(len(db))/48000,db,label=row['label'])
    axes[0,0].set(title='Space: measured impulse energy decay',xlabel='Seconds',ylabel='Remaining energy (dB)',xlim=(0,12),ylim=(-80,2));axes[0,0].legend()
    for row,audio,_ in series['decay-eq']:
        db,_=decay(audio,48000,1000);axes[0,1].plot(np.arange(len(db))/48000,db,label=row['label'])
    axes[0,1].set(title='Decay EQ: 1 kHz octave decay',xlabel='Seconds',ylabel='Remaining band energy (dB)',xlim=(0,8),ylim=(-80,2));axes[0,1].legend()
    for row,audio,_ in series['post-eq']:
        spectrum=np.sqrt(np.sum(np.abs(np.fft.rfft(audio.astype(np.float64),axis=0))**2,axis=1))
        freq=np.fft.rfftfreq(len(audio),1/48000);axes[1,0].semilogx(freq,20*np.log10(np.maximum(spectrum,1e-12)),label=row['label'],linewidth=.8)
    axes[1,0].set(title='Post EQ: measured wet magnitude',xlabel='Frequency (Hz)',ylabel='Impulse magnitude (dB)',xlim=(50,16000));axes[1,0].legend()
    for row,audio,_ in series['predelay']:
        if row['single_parameter_variation']:axes[1,1].plot(np.arange(7200)/48000,audio[:7200,0],label=row['label'],linewidth=.8)
    axes[1,1].set(title='Predelay: measured wet onset',xlabel='Seconds',ylabel='Left sample amplitude',xlim=(0,.15));axes[1,1].legend()
    for axis in axes.flat:axis.grid(alpha=.2)
    fig.suptitle('Installed FabFilter Pro-R 2.06 — controlled 48 kHz / 512-frame impulse observations',fontsize=13)
    fig.savefig(args.output/'reference-observations.png',dpi=160);plt.close(fig)
    shutil.copyfile(inspect,args.output/'parameter-dump.json')
    output={'status':'PASS_RETAINED_REFERENCE_ANALYSIS','plugin':inspection['plugin_before'],
            'inspection_sha256':sha(inspect),'parameter_count':inspection['inspection_prepared_48000_512']['parameter_count'],
            'render_count':len(records),'records':records,
            'scope':'Observed impulse responses and descriptive T20 extrapolations only. No sonic parity, proprietary equations, product CPU or human acceptance follows. All raw WAVs remain unchanged/private.',
            'limitations':['Finite 24-second observation; T20 is a fitted extrapolation, not a universal decay guarantee.',
                           'Fresh Modern baseline uses explicit defaults, wet Mix100%, no auto gate/freeze/predelay and Character0; complete applied settings are retained per capture.',
                           'Impulse-only ducking observations do not establish the adaptive detector law or full ballistics.',
                           'Reference reports nonfinite tail metadata; exact kind is retained.']}
    (args.output/'analysis.json').write_text(json.dumps(output,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'status':output['status'],'renders':len(records),'parameter_count':output['parameter_count'],'output':str(args.output)}))


if __name__=='__main__':main()
