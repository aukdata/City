from pathlib import Path
import csv, statistics, json
ROOT = Path(__file__).parent
def stats(values):
    v = sorted(values)
    return dict(mean=round(statistics.mean(v),3),p95=v[min(len(v)-1,int(len(v)*.95))],max=v[-1])
result={}
for label in ['traffic_baseline','traffic_intermediate','traffic_final','traffic_perf_final']:
    path=ROOT/label/'playtest_frames.csv'
    if not path.exists(): continue
    rows=list(csv.DictReader(path.open(encoding='utf-8-sig')))
    result[label]={}
    for phase in ['1','2']:
        allphase=[r for r in rows if r['commandId']==phase]
        sample=allphase[1200:2400]
        keys=['renderMs','frameMs','carUpdateMs','carDrawMs','roadMs','flushMs','gpuMs','systemUpdateMs','roadCandidates','nodeCandidates','minimapMs']
        data={k:stats([float(r[k]) for r in sample]) for k in keys if k in sample[0]}
        data['coldRenderMax']=max(float(r['renderMs']) for r in allphase)
        data['cars']=[min(int(r['cars']) for r in sample),max(int(r['cars']) for r in sample)]
        data['steadySamples']=len(sample)
        data['renderOver40']=sum(float(r['renderMs'])>40 for r in allphase)
        result[label][phase]=data
(ROOT/'traffic_comparison.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
for label, phases in result.items():
    for phase, values in phases.items():
        print(label,phase,values['cars'],{k:values[k] for k in ['renderMs','carUpdateMs','frameMs','roadMs']},'coldMax',values['coldRenderMax'])
