import argparse
import csv
from collections import defaultdict
from html import escape
import math
from pathlib import Path
import re
import statistics as stats


def plot(path, title, xlabel, ylabel, series, logx=False):
    points = [p for _, values in series for p in values]
    if not points:
        return
    xmin, xmax = min(p[0] for p in points), max(p[0] for p in points)
    if logx:
        xmin, xmax = math.log2(xmin), math.log2(xmax)
    if xmin == xmax:
        xmax = xmin + 1
    ymax = max(1e-9, max(p[3] if len(p) == 4 else p[1] for p in points)) * 1.12
    def number(value):
        digits = 0 if value >= 10 or value == int(value) else 2
        return f'{value:.{digits}f}'.replace('.', ',')
    def x(v):
        v = math.log2(v) if logx else v
        return 115 + (v-xmin)/(xmax-xmin)*680
    def y(v):
        return 400-v/ymax*325
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="500" viewBox="0 0 1100 500">',
           '<rect width="1100" height="500" fill="white"/>',
           '<g font-family="Arial,sans-serif" font-size="16" fill="#17233b">',
           f'<text x="115" y="30" font-size="22">{escape(title)}</text>',
           '<path d="M115 65 V400 H795" stroke="#17233b" fill="none"/>',
           f'<text x="455" y="473" text-anchor="middle">{escape(xlabel)}</text>',
           f'<text transform="translate(26,240) rotate(-90)" text-anchor="middle">{escape(ylabel)}</text>']
    for value in sorted(set(p[0] for p in points)):
        svg.append(f'<text x="{x(value):.1f}" y="426" text-anchor="middle">{number(value)}</text>')
    for i in range(6):
        value = ymax*i/5
        svg.append(f'<path d="M115 {y(value):.1f} H795" stroke="#e7ebf0"/>')
        svg.append(f'<text x="100" y="{y(value)+5:.1f}" text-anchor="end">{number(value)}</text>')
    colors = ['#2563eb','#dc2626','#059669','#9333ea','#d97706','#0891b2','#475569','#be185d']
    for i, (label, values) in enumerate(series):
        color = colors[i % len(colors)]
        coordinates = ' '.join(f'{x(p[0]):.1f},{y(p[1]):.1f}' for p in sorted(values))
        svg.append(f'<polyline points="{coordinates}" fill="none" stroke="{color}" stroke-width="2"/>')
        for point in values:
            a,b = point[:2]
            if len(point) == 4:
                low, high = point[2:]
                px, bottom, top = x(a), y(low), y(high)
                svg.append(f'<path d="M{px:.1f} {top:.1f} V{bottom:.1f} M{px-4:.1f} {top:.1f} H{px+4:.1f} M{px-4:.1f} {bottom:.1f} H{px+4:.1f}" stroke="{color}" stroke-width="1.5" fill="none" opacity="0.65"/>')
            svg.append(f'<circle cx="{x(a):.1f}" cy="{y(b):.1f}" r="3" fill="{color}"/>')
        svg.append(f'<text x="820" y="{85+27*i}" fill="{color}">{escape(label)}</text>')
    svg.append('<text x="820" y="370" font-size="14">Точки — медіани</text>')
    svg.append('<text x="820" y="391" font-size="14">Смуги — min–max</text>')
    svg += ['</g></svg>']
    path.write_text('\n'.join(svg), encoding='utf-8')


def write_findings(out, rows, summaries, fixed_p, scaling_k):
    text = ['# Measurement observations', '',
            'Generated from this directory\'s results.csv. This is a data summary, not a final report.', '',
            'All speedups use the median V0 time. V1 values are incorrect-output diagnostics; '
            'its serial preparation is included and its work differs from the correct versions.', '',
            '## Scaling', '',
            '| Version | P | Median ms | Min–max ms | Speedup | Efficiency | Karp–Flatt |',
            '|---|---:|---:|---:|---:|---:|---:|']
    scaling = [s for s in summaries if s['experiment'] in ('scaling','index')
               and s['version'] in ('v0','v1','v2','v3','v4')
               and (s['version']!='v3' or int(s['K'])==scaling_k)]
    for s in scaling:
        label = s['version'] + ('-'+s['merge'] if s['version']=='v4' else '')
        if s['version']=='v3':
            label += '-K'+s['K']
        if s['version']=='v1':
            label += ' (diagnostic)'
        def fmt(value):
            return f'{value:.3f}' if isinstance(value,(float,int)) else '—'
        text.append(f"| {label} | {s['threads']} | {s['median_ms']:.3f} | "
                    f"{s['min_ms']:.3f}–{s['max_ms']:.3f} | {fmt(s['speedup'])} | "
                    f"{fmt(s['efficiency'])} | {fmt(s['karp_flatt'])} |")
    log = out/'run.log'
    if log.exists():
        match = re.search(r'top20_occurrences=(\d+)\s+top20_share=([\d.]+)',
                          log.read_text(encoding='utf-8-sig',errors='replace'))
        if match:
            text += ['', f'Top-20 words: {int(match[1]):,} updates, {100*float(match[2]):.3f}% of accepted occurrences.',
                     'Increasing K separates hash collisions, but cannot separate updates to the same frequent word. '
                     'The combined top-20 share is not itself an Amdahl serial fraction: those words may occupy different shards.']
    baseline = next((int(r['total_occurrences']) for r in rows if r['version']=='v0'),None)
    repro = [r for r in rows if r['experiment']=='v1_repro']
    if baseline is not None and repro:
        totals = [int(r['total_occurrences']) for r in repro]
        text += ['', '## V1 repeated runs', '',
                 f'{len(repro)} retained runs; V0 total {baseline:,}; observed totals '
                 f'{min(totals):,}–{max(totals):,}; lost updates '
                 f'{baseline-max(totals):,}–{baseline-min(totals):,}. See v1-runs.csv.',
                 'Observed unsafe-mode results do not establish portable guarantees about undefined behavior.']
    sweep = sorted((s for s in summaries if s['experiment']=='v3_sweep' and int(s['threads'])==fixed_p),key=lambda s:int(s['K']))
    if sweep:
        text += ['', f'## V3 sweep, P={fixed_p}', '',
                 '| K | Median ms | Min–max ms |', '|---:|---:|---:|']
        for s in sweep:
            text.append(f"| {s['K']} | {s['median_ms']:.3f} | {s['min_ms']:.3f}–{s['max_ms']:.3f} |")
        candidate = min(sweep, key=lambda s:s['median_ms'])
        if candidate is sweep[-1]:
            text += ['', 'No plateau was established before the largest tested K.']
        else:
            text += ['', f"The lowest observed median is at K={candidate['K']}; later K values do not improve it. "
                     f"The measured curve therefore levels off around K={candidate['K']}. "
                     'This is a point estimate, not proof of a unique optimum; overlapping ranges show uncertainty. Inspect shards.svg.']
    merges = {(s['threads'],s['merge']):s for s in scaling if s['version']=='v4'}
    if merges:
        text += ['', '## V4 merge phase', '',
                 '| P | Sequential median ms | Tree median ms | Sequential/tree ratio |',
                 '|---:|---:|---:|---:|']
        for p in sorted({key[0] for key in merges}, key=int):
            a,b = merges.get((p,'sequential')),merges.get((p,'tree'))
            if a and b:
                ratio = f"{a['merge_median_ms']/b['merge_median_ms']:.3f}" if int(p)>1 and b['merge_median_ms'] else '—'
                text.append(f"| {p} | {a['merge_median_ms']:.3f} | {b['merge_median_ms']:.3f} | {ratio} |")
        text += ['', 'Tree rounds reduce dependency depth, but add work, thread launches and memory traffic; '
                 'logarithmic depth does not guarantee a lower elapsed time. P=1 has no meaningful merge work.']
    sharing = {(s['threads'],s['version']):s for s in summaries if s['experiment']=='false_sharing'}
    if sharing:
        text += ['', '## False sharing', '',
                 '| P | Adjacent median ms | Padded median ms | Adjacent/padded ratio |',
                 '|---:|---:|---:|---:|']
        for p in sorted({key[0] for key in sharing},key=int):
            a,b = sharing.get((p,'adjacent')),sharing.get((p,'padded'))
            if a and b:
                ratio = a['median_ms']/b['median_ms'] if b['median_ms'] else 0
                text.append(f"| {p} | {a['median_ms']:.3f} | {b['median_ms']:.3f} | {ratio:.3f} |")
        text += ['', 'Each thread owns a different counter, so these accesses have no data race. '
                 'Adjacent counters share cache-line ownership and cause coherence invalidations; '
                 'line-sized stride/alignment removes that sharing. Verify the measured line size in environment.txt. '
                 'Oversubscription and scheduling still affect the padded case; see the raw min/max ranges.']
    text += ['', '## Interpretation limits', '',
             '- Check environment.txt, machine.txt and power-status.json before attributing changes solely to the algorithm.',
             '- Physical cores, SMT, oversubscription, allocation, bandwidth and system load all affect scaling.',
             '- Amdahl: S(P) <= 1/(s+(1-s)/P), S(infinity)=1/s for a fixed serial fraction s.',
             '- Karp–Flatt estimates include overhead. Negative estimates or estimates above one are not literal serial fractions; '
             'reciprocal estimates in summary.csv are effective extrapolations, not guaranteed bounds.',
             '- Repetitions are grouped by configuration, not randomized; thermal drift is a possible source of bias.']
    short = [s for s in summaries if s['experiment']!='v1_repro' and s['samples']<5]
    if short:
        text += [f'- {len(short)} configurations have fewer than five samples: these are incomplete or diagnostic measurements.']
    (out/'findings.md').write_text('\n'.join(text)+'\n',encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--scaling-k', type=int, default=64)
    parser.add_argument('--fixed-p', type=int, default=8)
    args = parser.parse_args()
    with args.csv.open(newline='', encoding='utf-8-sig') as source:
        rows = list(csv.DictReader(source))
    if not rows:
        parser.error('CSV contains no measurements')
    out = args.csv.parent
    groups = defaultdict(list)
    for row in rows:
        key = tuple(row[k] for k in ('experiment','version','threads','K','merge','v1_mode'))
        groups[key].append(row)
        if row['version'] in ('v0','v2','v3','v4') and row['correct'] != '1':
            raise ValueError('Refusing to analyze an incorrect supposedly correct version')
    summaries = []
    for key, values in sorted(groups.items()):
        times = [float(r['compute_ms']) for r in values]
        summaries.append(dict(zip(('experiment','version','threads','K','merge','v1_mode'), key),
                              samples=len(times), median_ms=stats.median(times), min_ms=min(times), max_ms=max(times),
                              merge_median_ms=stats.median(float(r['merge_ms']) for r in values),
                              merge_min_ms=min(float(r['merge_ms']) for r in values),
                              merge_max_ms=max(float(r['merge_ms']) for r in values)))
    baseline = [s['median_ms'] for s in summaries if s['version']=='v0']
    t0 = baseline[0] if baseline else None
    for row in summaries:
        p = int(row['threads'])
        valid_index = row['version'] in ('v0','v1','v2','v3','v4')
        speedup = t0/row['median_ms'] if t0 is not None and row['median_ms'] > 0 and valid_index else None
        row['speedup'] = speedup if speedup is not None else ''
        row['efficiency'] = speedup/p if speedup is not None else ''
        e = (1/speedup-1/p)/(1-1/p) if speedup and p>1 else None
        row['karp_flatt'] = e if e is not None else ''
        row['amdahl_effective_limit'] = 1/e if e is not None and 0<e<=1 else ''
        row['interpretation'] = 'incorrect-output diagnostic; prepass included' if row['version']=='v1' else 'correct'
    with (out/'summary.csv').open('w', newline='', encoding='utf-8') as target:
        writer = csv.DictWriter(target, fieldnames=list(summaries[0]))
        writer.writeheader(); writer.writerows(summaries)
    speed, efficiency, shards, sharing, merges = (defaultdict(list) for _ in range(5))
    ps = set()
    for row in summaries:
        v, p, k = row['version'], int(row['threads']), int(row['K'])
        label = v.upper() + (' '+{'sequential':'послідовне','tree':'дерево'}[row['merge']] if v=='v4' else '')
        if v=='v3':
            label += f'-K{k}'
        if v=='v1':
            label += ' '+row['v1_mode']+' (некоректна)'
        if row['experiment'] in ('index','scaling') and row['speedup'] != '' and (v!='v3' or k==args.scaling_k):
            low, high = t0/row['max_ms'], t0/row['min_ms']
            speed[label].append((p,row['speedup'],low,high))
            efficiency[label].append((p,row['efficiency'],low/p,high/p))
            ps.add(p)
        if v=='v3' and p==args.fixed_p and row['experiment']=='v3_sweep':
            shards['V3, P='+str(args.fixed_p)].append((k,row['median_ms']/1000,row['min_ms']/1000,row['max_ms']/1000))
        if row['experiment']=='false_sharing':
            sharing[{'adjacent':'A — сусідні','padded':'B — рознесені'}[v]].append((p,row['median_ms'],row['min_ms'],row['max_ms']))
        if v=='v4' and row['experiment'] in ('index','scaling'):
            merges[{'sequential':'Послідовне','tree':'Дерево'}[row['merge']]].append((p,row['merge_median_ms']/1000,row['merge_min_ms']/1000,row['merge_max_ms']/1000))
    if ps:
        speed.pop('V0', None)
        speed['Ідеальне S(P)=P'] = [(p,float(p)) for p in sorted(ps)]
        speed['V0 — база S=1'] = [(p,1.0) for p in sorted(ps)]
    plot(out/'speedup.svg','Прискорення відносно V0','Кількість потоків P','S(P)',list(speed.items()))
    plot(out/'efficiency.svg','Паралельна ефективність','Кількість потоків P','E(P)',list(efficiency.items()))
    plot(out/'shards.svg',f'Секціонування V3 при P={args.fixed_p}','K (логарифмічна шкала)','Час обчислення, с',list(shards.items()),logx=True)
    plot(out/'false-sharing.svg','Хибне розділення','Кількість потоків P','Час, мс',list(sharing.items()))
    plot(out/'merge.svg','Злиття локальних індексів V4','Кількість потоків P','Час злиття, с',list(merges.items()))
    reference_total = next((int(r['total_occurrences']) for r in rows if r['version']=='v0'), None)
    repro = [r for r in rows if r['experiment']=='v1_repro']
    if repro and reference_total is not None:
        with (out/'v1-runs.csv').open('w',newline='',encoding='utf-8') as target:
            writer=csv.writer(target)
            writer.writerow(['run','mode','threads','reference_total','observed_total','lost','loss_percent','equal_reference'])
            for r in repro:
                observed=int(r['total_occurrences']); lost=reference_total-observed
                writer.writerow([r['repeat'],r['v1_mode'],r['threads'],reference_total,observed,lost,
                                 100*lost/reference_total if reference_total else 0,r['correct']])
        totals = [int(r['total_occurrences']) for r in repro]
        print(f'V1: {len(repro)} runs; total range {min(totals)}..{max(totals)}, V0={reference_total}')
    write_findings(out, rows, summaries, args.fixed_p, args.scaling_k)
    print(f'Summary and available SVG plots written to {out}')
    print('Error spread is in summary.csv. Effective Amdahl limits include overhead; V1 metrics are diagnostic only.')


if __name__ == '__main__':
    main()
