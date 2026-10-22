import csv
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', nargs='?', default='build')
parser.add_argument('--results', type=Path, help='Retain measurement/plot checks here (test data only)')
args = parser.parse_args()
build = (root / args.build).resolve()
exe = build / ('indexer.exe' if sys.platform == 'win32' else 'indexer')
sharing = exe.with_name('false_sharing.exe' if sys.platform == 'win32' else 'false_sharing')


def run(command, ok=True):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                            encoding='utf-8', errors='replace', timeout=60)
    if (result.returncode == 0) != ok:
        raise AssertionError(f'Unexpected exit {result.returncode}: {command}\n{result.stdout}\n{result.stderr}')
    return result


with tempfile.TemporaryDirectory(prefix='cli-smoke-', dir=build) as temp:
    work = Path(temp)
    corpus = work/'corpus'
    corpus.mkdir()
    if args.results:
        work = args.results.resolve()
        work.mkdir(parents=True, exist_ok=True)
    for i in range(16):
        (corpus/f'{i:02}.txt').write_text('Apple banana apple КІТ 123.\n'*1000, encoding='utf-8')
    reference = work/'reference.tsv'
    output = work/'output.tsv'
    raw = work/'results.csv'
    common = [exe,'--input',corpus,'--output',output]
    if raw.exists():
        raise ValueError('Use an empty --results directory')
    run([exe,'--input',corpus,'--output',reference,'--version','v0','--repeats','5','--csv',raw,'--experiment','scaling'])
    for version in ('v2','v3','v4'):
        for method in (('sequential','tree') if version=='v4' else ('sequential',)):
            run(common+['--version',version,'--threads','3','--merge',method,'--verify',reference,
                        '--repeats','5','--csv',raw,'--experiment','scaling'])
            assert output.read_bytes()==reference.read_bytes()
    for k in (1,4,16,64,256,1024,4096):
        run(common+['--version','v3','--threads','3','--shards',str(k),'--verify',reference,
                    '--csv',raw,'--experiment','v3_sweep'])
    for repeat in range(1,11):
        run(common+['--version','v1','--threads','4','--verify',reference,'--csv',raw,
                    '--experiment','v1_repro','--repeat-start',str(repeat)])
    run(common+['--version','v1','--threads','3','--verify',reference,'--csv',raw,'--experiment','scaling'])
    run([sharing,'--repeats','1','--iterations','10000','--csv',raw])
    run([sys.executable,root/'scripts/analyze.py',raw,'--fixed-p','3'])
    for name in ('speedup','efficiency','shards','false-sharing','merge'):
        ET.parse(work/f'{name}.svg')
    with (work/'v1-runs.csv').open(newline='') as source:
        v1=list(csv.DictReader(source))
    assert len(v1)==10 and {r['run'] for r in v1}=={str(n) for n in range(1,11)}
    assert all(int(r['observed_total'])<=int(r['reference_total']) for r in v1)
    for extra in (['--threads','0'], ['--version','v9'], ['--unknown','x'], ['--merge','invalid'],
                  ['--require-corpus','yes']):
        run(common+extra,ok=False)
    run([exe,'--input',corpus,'--output',corpus/'contamination.tsv'],ok=False)
    run([exe,'--input',corpus,'--output',reference,'--verify',reference],ok=False)
    run(common+['--csv',output],ok=False)
    run(common+['--csv',str(output)+'.documents.tsv'],ok=False)
    badref=work/'bad-reference.tsv'
    badref.write_text('incorrect reference',encoding='utf-8')
    no_csv=work/'must-not-exist.csv'
    run(common+['--version','v2','--verify',badref,'--csv',no_csv],ok=False)
    assert not no_csv.exists(), 'Bad reference must fail before measured CSV rows'
    invalid=work/'invalid'
    invalid.mkdir()
    (invalid/'bad.txt').write_bytes(b'\x80')
    run([exe,'--input',invalid,'--output',output,'--version','v4','--threads','3'],ok=False)
    print('CLI checks passed: exact files, CSV, ten V1 runs, five SVG plots, invalid inputs, preflight gate.')
    print('V1 test-corpus loss range:',min(int(r['lost']) for r in v1),'..',max(int(r['lost']) for r in v1))
    if args.results:
        (work/'NOTICE.txt').write_text('Generated correctness/stress test inputs, NOT the natural-language assignment corpus. Timings are diagnostic only.\n',encoding='utf-8')
