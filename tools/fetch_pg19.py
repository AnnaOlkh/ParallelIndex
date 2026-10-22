import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
from urllib.parse import urlencode
from urllib.request import Request, urlopen

SOURCE = 'https://github.com/google-deepmind/pg19'
BUCKET = 'https://storage.googleapis.com/deepmind-gutenberg/'
LIST_API = 'https://storage.googleapis.com/storage/v1/b/deepmind-gutenberg/o'
MIB = 1024 * 1024
MAX_BOOK = MIB


def get(url, limit):
    request = Request(url, headers={'User-Agent': 'ParallelComputingLab/1.0 (PG19 educational corpus)'})
    with urlopen(request, timeout=60) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError('Response exceeds approved per-request size limit')
    return data


def candidates():
    token = None
    for _ in range(50):
        query = {'prefix': 'train/', 'maxResults': 1000,
                 'fields': 'items(name,size,md5Hash),nextPageToken'}
        if token:
            query['pageToken'] = token
        page = json.loads(get(LIST_API + '?' + urlencode(query), 4*MIB))
        for item in sorted(page.get('items', []), key=lambda item: item['name']):
            name = item['name']
            size = int(item['size'])
            if re.fullmatch(r'train/[0-9]+\.txt', name) and 8192 <= size <= MAX_BOOK:
                yield item
        token = page.get('nextPageToken')
        if not token:
            return
    raise ValueError('Unexpectedly large metadata listing; stopping')


def download(destination, metadata, target_mib):
    if destination == metadata or destination in metadata.parents or metadata in destination.parents:
        raise ValueError('Corpus and metadata directories must be separate')
    if destination.exists() and any(destination.iterdir()):
        raise ValueError('Destination must be empty; existing data will not be overwritten')
    if metadata.exists() and any(metadata.iterdir()):
        raise ValueError('Metadata directory must be empty; choose a fresh destination')
    destination.mkdir(parents=True, exist_ok=True)
    metadata.mkdir(parents=True, exist_ok=True)
    records = []
    total = 0
    target = target_mib*MIB
    for item in candidates():
        url = BUCKET + item['name']
        data = get(url, MAX_BOOK)
        if len(data) != int(item['size']):
            raise ValueError('Object size changed; stop to preserve the approved budget')
        data.decode('utf-8', errors='strict')
        if item.get('md5Hash'):
            digest = base64.b64encode(hashlib.md5(data).digest()).decode('ascii')
            if digest != item['md5Hash']:
                raise ValueError('Source MD5 does not match downloaded object')
        filename = Path(item['name']).name
        with (destination/filename).open('xb') as output:
            output.write(data)
        total += len(data)
        records.append({'file': filename, 'url': url, 'bytes': len(data),
                        'source_md5_base64': item.get('md5Hash'),
                        'sha256': hashlib.sha256(data).hexdigest()})
        manifest = {'dataset': 'PG-19 training subset', 'source': SOURCE,
                    'retrieved_utc': datetime.now(timezone.utc).isoformat(),
                    'selection': 'lexicographic object order, 8 KiB..1 MiB books, stop at target bytes',
                    'license_source': 'https://huggingface.co/datasets/deepmind/pg19',
                    'target_mib': target_mib, 'files': len(records), 'bytes': total,
                    'complete': total >= target and len(records) >= 200, 'objects': records}
        (metadata/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
        print(f'books={len(records)} MiB={total/MIB:.2f}', flush=True)
        if manifest['complete']:
            return manifest
    raise ValueError('Not enough suitable objects to meet the assignment minimum')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('corpus/pg19'))
    parser.add_argument('--metadata', type=Path, default=Path('corpus-info/pg19'))
    parser.add_argument('--target-mib', type=int, default=220)
    parser.add_argument('--download', action='store_true', help='Actually access network and download; requires user approval')
    args = parser.parse_args()
    if not 200 <= args.target_mib <= 1024:
        parser.error('target-mib must be in 200..1024')
    print(f'Source: {SOURCE}\nCorpus: {args.output}\nMetadata: {args.metadata}')
    print(f'Book-data budget: {args.target_mib} to <{args.target_mib+1} MiB; metadata additional.')
    if not args.download:
        print('OFFLINE PLAN ONLY: no network requests or downloads. Add --download only after approval.')
        return
    try:
        result = download(args.output.resolve(), args.metadata.resolve(), args.target_mib)
    except Exception as error:
        print(f'Download stopped: {error}. Partial files/manifest are retained.', file=sys.stderr)
        raise SystemExit(1) from error
    print(f'Ready: {result["files"]} files, {result["bytes"]} bytes. Preserve manifest.json.')


if __name__ == '__main__':
    main()
