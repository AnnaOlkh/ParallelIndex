import argparse
import csv
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics
import xml.etree.ElementTree as ET


def check(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    with path.open(newline='',encoding='utf-8-sig') as source:
        return list(csv.DictReader(source))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv',type=Path)
    args = parser.parse_args()
    rows = read(args.csv)
    out = args.csv.parent
    groups = defaultdict(list)
    fields = ('experiment','version','threads','K','merge','v1_mode')
    for r in rows:
        groups[tuple(r[f] for f in fields)].append(r)
        for field in ('build_ms','merge_ms','canonicalize_ms','compute_ms','read_ms','write_ms'):
            check(math.isfinite(float(r[field])) and float(r[field])>=0,'Invalid time')
        check(math.isclose(float(r['compute_ms']),sum(float(r[f]) for f in ('build_ms','merge_ms','canonicalize_ms')),
                           rel_tol=0,abs_tol=.001),'Phase times do not sum to compute time')
    expected = {('scaling','v0','1','0','na','na'), ('v1_repro','v1','8','0','na','unsafe')}
    for p in ('1','2','4','8'):
        expected.update({('scaling','v1',p,'0','na','unsafe'),('scaling','v2',p,'0','na','na'),
                         ('scaling','v3',p,'64','na','na'),('scaling','v4',p,'0','sequential','na'),
                         ('scaling','v4',p,'0','tree','na'),('false_sharing','adjacent',p,'0','na','na'),
                         ('false_sharing','padded',p,'0','na','na')})
    expected.update(('v3_sweep','v3','8',str(k),'na','na') for k in (1,4,16,64,256,1024,4096))
    check(set(groups)==expected,'Missing/unexpected configuration; this audit targets the default full campaign')
    for key,values in groups.items():
        need = 10 if key[0]=='v1_repro' else 5
        check(len(values)>=need,'Too few repetitions: '+str(key))
        check(len({v['repeat'] for v in values})==len(values),'Duplicate repetition IDs: '+str(key))
    baseline_rows = groups[('scaling','v0','1','0','na','na')]
    baseline_total = int(baseline_rows[0]['total_occurrences'])
    t0 = statistics.median(float(r['compute_ms']) for r in baseline_rows)
    files, size = int(baseline_rows[0]['files']),int(baseline_rows[0]['bytes'])
    check(files>=200 and size>=200*1024*1024,'Nonqualifying corpus')
    for r in rows:
        if r['experiment']=='false_sharing':
            check(r['correct']=='1' and int(r['total_occurrences'])==int(r['threads'])*10000000,
                  'False-sharing counter count or verification failed')
        else:
            check(int(r['files'])==files and int(r['bytes'])==size,'Corpus changed between samples')
            if r['version'] in ('v0','v2','v3','v4'):
                check(r['correct']=='1' and int(r['total_occurrences'])==baseline_total,'Incorrect parallel output')
    summary = read(out/'summary.csv')
    check(len(summary)==len(groups),'Summary/configuration count mismatch')
    for s in summary:
        key = tuple(s[f] for f in fields)
        values = [float(r['compute_ms']) for r in groups[key]]
        check(int(s['samples'])==len(values),'Wrong sample count in summary')
        for field,want in (('median_ms',statistics.median(values)),('min_ms',min(values)),('max_ms',max(values))):
            check(math.isclose(float(s[field]),want,rel_tol=1e-10),'Incorrect summary statistic')
        if s['version'] in ('v0','v1','v2','v3','v4'):
            speed = t0/statistics.median(values)
            check(math.isclose(float(s['speedup']),speed,rel_tol=1e-10),'Speedup must use V0')
            check(math.isclose(float(s['efficiency']),speed/int(s['threads']),rel_tol=1e-10),'Efficiency error')
    for name in ('speedup','efficiency','shards','false-sharing','merge'):
        tree = ET.parse(out/f'{name}.svg')
        check(tree.getroot().tag=='{http://www.w3.org/2000/svg}svg','Invalid SVG')
    repro = read(out/'v1-runs.csv')
    check(len(repro)==10 and {int(r['run']) for r in repro}==set(range(1,11)),'V1 requires ten separately numbered runs')
    for r in repro:
        check(int(r['reference_total'])==baseline_total,'V1 reference total error')
        check(int(r['lost'])==baseline_total-int(r['observed_total']),'V1 loss calculation error')
    check(any(int(r['lost'])!=0 for r in repro),'V1 incorrectness did not manifest')
    sweep = [s for s in summary if s['experiment']=='v3_sweep']
    best = min(sweep,key=lambda s:float(s['median_ms']))
    report = {'status':'passed','raw_samples':len(rows),'configurations':len(groups),
              'files':files,'bytes':size,'reference_occurrences':baseline_total,
              'v1_repro_runs':len(repro),'lowest_v3_sweep_median_K':int(best['K']),
              'plots_verified':5}
    (out/'audit.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
