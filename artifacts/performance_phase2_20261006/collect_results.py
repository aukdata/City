from pathlib import Path
import json, shutil, re
root=Path(__file__).parent
runs=Path('artifacts/local_runs/checks')
mapping={
 'navigation_baseline':'20261006-191427-playtest-r_hgggsx',
 'navigation_final':'20261006-201144-playtest-rpxw_hfo',
 'streaming_previous':'20261006-161329-playtest-kptx5mfm',
 'streaming_final':'20261006-201503-playtest-76tem_nb',
}
for label, run in mapping.items():
    out=root/label; out.mkdir(exist_ok=True)
    for filename in ['summary.json','debug.log','navigation_frames.csv','streaming_frames.csv']:
        source=runs/run/filename
        if source.exists(): shutil.copy2(source,out/filename)
baseline=(root/'navigation_baseline/debug.log').read_text(encoding='utf-8-sig')
final=(root/'navigation_final/debug.log').read_text(encoding='utf-8-sig')
uploads={}
for label, text in [('baseline',baseline),('final',final)]:
    values=[float(v) for v in re.findall(r'maxFrameUploadMs=([0-9.]+)',text)]
    uploads[label]={'publications':len(values),'maxFrameUploadMs':max(values)}
(root/'upload_comparison.json').write_text(json.dumps(uploads,indent=2),encoding='utf-8')
print('Collected navigation and previous streaming records;',uploads)
