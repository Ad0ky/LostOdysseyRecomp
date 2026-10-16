import io
import unittest
import zipfile

import logs

LOG = ('[    0.1 t1] [note]  source version: 0.9.52\n[    0.2 t1] [note]  settings: backend=1\n'
       '[    1.0 t2] [warn]  xma failed offset=100\n[    2.0 t2] [warn]  xma failed offset=200\n'
       '[    3.0 t2] [error] boom 0x10\n')


def make_zip(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


class LogTests(unittest.TestCase):
    def test_attachments_newest_first_and_types(self):
        body = ('[a](https://github.com/user-attachments/files/1/runtime-100.log) '
                'https://github.com/user-attachments/files/2/runtime-300.log '
                'https://github.com/user-attachments/files/3/logs.zip '
                'https://github.com/user-attachments/files/4/x.png')
        urls = logs.attachments(body)
        self.assertEqual([u.rsplit('/', 1)[1] for u in urls], ['runtime-300.log', 'runtime-100.log', 'logs.zip'])
        self.assertTrue(logs.unsupported('https://github.com/user-attachments/files/9/logs.7z'))

    def test_digest_dedupes_and_keeps_header(self):
        text = logs.digest(LOG)
        self.assertIn('source version: 0.9.52', text)
        self.assertIn('x2 [    1.0 t2] [warn]  xma failed', text)
        self.assertIn('x1 [    3.0 t2] [error]', text)

    def test_zip_selects_newest_runtime_logs(self):
        raw = make_zip({'logs/runtime-100.log': LOG, 'logs/runtime-300.log': LOG, 'logs/runtime-200.log': LOG,
                        'logs/other.txt': LOG, 'logs/sub/': ''})
        self.assertEqual([n for n, _ in logs.zip_logs(raw)], ['logs/runtime-300.log', 'logs/runtime-200.log'])

    def test_zip_falls_back_to_other_logs(self):
        raw = make_zip({'a.txt': LOG, 'b.png': 'x'})
        self.assertEqual([n for n, _ in logs.zip_logs(raw)], ['a.txt'])

    def test_zip_guards(self):
        big = make_zip({'runtime-900.log': 'x' * (logs.MAX_MEMBER + 1), 'runtime-100.log': LOG})
        self.assertEqual([n for n, _ in logs.zip_logs(big)], ['runtime-100.log'])
        bomb = make_zip({'runtime-900.log': '0' * 5_000_000, 'runtime-100.log': LOG})
        self.assertEqual([n for n, _ in logs.zip_logs(bomb)], ['runtime-100.log'])
        nested = make_zip({'inner.zip': make_zip({'runtime-1.log': LOG}).decode('latin-1')})
        self.assertEqual(logs.zip_logs(nested), [])
        with self.assertRaises(logs.LogError):
            logs.zip_logs(b'not a zip')

    def test_collect_labels_zip_members_and_skips_failures(self):
        zip_url = 'https://github.com/user-attachments/files/3/logs.zip'
        bad_url = 'https://github.com/user-attachments/files/4/runtime-5.log'
        raw = make_zip({'logs/runtime-100.log': LOG})

        def fetch(url, limit=logs.MAX_DOWNLOAD):
            if url == bad_url:
                raise logs.LogError('no')
            return raw
        result = logs.collect(f'{zip_url} {bad_url}', fetch)
        self.assertEqual([r['file'] for r in result], ['logs.zip/logs/runtime-100.log'])

    def test_version_comes_from_log_not_path(self):
        log = ('[    0.0 t1] [note]  source version: 0.9.52\n'
               '[    0.1 t1] [note]  build: source=0.9.52 revision=9769f12e5293\n'
               '[    0.2 t1] [note]  game folder: D:\\Games\\LostOdysseyRecomp-windows-x64-v0.5.11\\logs\n')
        text = logs.digest(log)
        self.assertIn('Header facts:\ngame version (from log): 0.9.52 (revision 9769f12e5293)', text)
        self.assertEqual(logs.log_version(log), ('0.9.52', '9769f12e5293'))
        summary = logs.version_summary([{'file': 'runtime-1.log', 'version': logs.log_version(log)}])
        self.assertEqual(summary, 'game version (from log): 0.9.52 (revision 9769f12e5293)')

    def test_two_logs_label_each_version_and_newest(self):
        def fetch(url, limit=logs.MAX_DOWNLOAD):
            return LOG.replace('0.9.52', '0.9.0') if '/1/' in url else LOG
        body = ('https://github.com/user-attachments/files/1/runtime-100.log '
                'https://github.com/user-attachments/files/2/runtime-300.log')
        entries = logs.collect(body, fetch)
        summary = logs.version_summary(entries, 'source version: 0.9.0')
        self.assertEqual([e['file'] for e in entries], ['runtime-300.log', 'runtime-100.log'])
        self.assertIn('game version (from log): 0.9.52', summary)
        self.assertIn('runtime-300.log: 0.9.52; runtime-100.log: 0.9.0', summary)
        self.assertIn('Newest log: runtime-300.log', summary)
        self.assertIn('pastes a log excerpt saying 0.9.0', summary)

    def test_no_version_line_claims_nothing(self):
        log = '[    0.1 t1] [note]  settings: backend=1\n[    1.0 t2] [warn]  hmm\n'
        self.assertIsNone(logs.log_version(log))
        self.assertNotIn('game version', logs.digest(log))
        self.assertIsNone(logs.version_summary([{'file': 'a.log', 'version': None}], 'path LostOdysseyRecomp-v0.5.11'))

    def test_pasted_excerpt_used_when_no_attached_log(self):
        summary = logs.version_summary([], '```\n[ 0.1 t1] [note]  source version: 0.9.0\n```')
        self.assertIn('pasted in the issue): 0.9.0', summary)

    def test_redirect_outside_github_refused(self):
        with self.assertRaises(logs.LogError):
            logs.SameFamilyRedirect().redirect_request(None, None, 302, '', {}, 'https://evil.example/x')


if __name__ == '__main__':
    unittest.main()
