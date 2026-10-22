import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fetch_pg19', root/'tools/fetch_pg19.py')
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


class FetchTests(unittest.TestCase):
    def test_default_never_connects(self):
        with patch.object(sys, 'argv', ['fetch_pg19.py']), patch.object(fetch, 'get', side_effect=AssertionError('No network')):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                fetch.main()
        self.assertIn('OFFLINE PLAN ONLY', output.getvalue())

    def test_selection_filters_names_sizes(self):
        page = {'items': [{'name':'train/123.txt','size':'9000'},
                          {'name':'train/../../escape.txt','size':'9000'},
                          {'name':'train/124.txt','size':str(fetch.MAX_BOOK+1)},
                          {'name':'test/125.txt','size':'9000'}]}
        with patch.object(fetch, 'get', return_value=json.dumps(page).encode()):
            self.assertEqual([item['name'] for item in fetch.candidates()], ['train/123.txt'])

    def test_small_offline_transfer_and_existing_data_guard(self):
        build = root/'build'
        build.mkdir(exist_ok=True)
        data = b'example\n'
        items = [{'name':f'train/{n}.txt','size':str(len(data))} for n in range(200)]
        with tempfile.TemporaryDirectory(prefix='download-test-',dir=build) as temp:
            destination, metadata = Path(temp)/'text', Path(temp)/'info'
            with patch.object(fetch,'MIB',8), patch.object(fetch,'MAX_BOOK',8), \
                 patch.object(fetch,'candidates',return_value=iter(items)), patch.object(fetch,'get',return_value=data):
                with contextlib.redirect_stdout(io.StringIO()):
                    manifest = fetch.download(destination,metadata,200)
            self.assertTrue(manifest['complete'])
            self.assertEqual(manifest['files'],200)
            self.assertEqual(manifest['bytes'],1600)
            self.assertEqual(manifest['objects'][0]['sha256'],hashlib.sha256(data).hexdigest())
            self.assertEqual((destination/'0.txt').read_bytes(),data)
            with self.assertRaisesRegex(ValueError,'empty'):
                fetch.download(destination,metadata,200)


if __name__ == '__main__':
    unittest.main()
