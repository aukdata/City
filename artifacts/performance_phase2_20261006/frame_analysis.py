from pathlib import Path
import csv, json, math, statistics
root=Path(__file__).parent
rows=list(csv.DictReader((root/'traffic_perf_final/playtest_frames.csv').open(encoding='utf-8-sig')))
out={}
for phase in ['1','2']:
    group=[row for row in rows if row['commandId']==phase][1200:2400]
    data={}
    keys=['frameMs','renderMs','updateMs','systemUpdateMs','previousDiagnosticsMs','trafficSignalMs','trafficIDMMs','trafficLaneChangeMs','trafficOtherMs','roadCandidates','nodeCandidates']
    for key in keys:
        values=sorted(float(row[key]) for row in group)
        assert all(math.isfinite(value) for value in values)
        data[key]={'mean':round(statistics.mean(values),3),'p95':values[math.ceil(len(values)*.95)-1],'max':values[-1]}
    slow=[row for row in group if float(row['frameMs'])>25]
    data['framesOver25']=len(slow)
    data['framesOver25MeanStages']={key:round(statistics.mean(float(row[key]) for row in slow),3) for key in keys[:5]} if slow else {}
    data['unmeasuredMeanMs']=round(statistics.mean(float(r['frameMs'])-sum(float(r[k]) for k in ['renderMs','updateMs','systemUpdateMs','previousDiagnosticsMs']) for r in group),3)
    out[phase]=data
(root/'frame_diagnostics.json').write_text(json.dumps(out,indent=2),encoding='utf-8')
print(json.dumps(out,indent=2))
