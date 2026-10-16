from datetime import datetime, timezone
import unittest

import related

NOW = datetime(2026, 10, 14, 12, tzinfo=timezone.utc)
TEMPLATE = ('## Log\n\nAttach the latest log from the affected run. Logs are in the folder.\n\n'
            '[runtime-1.log](https://github.com/user-attachments/files/1/runtime-1.log)\n\n'
            '<!-- Drag and drop files here. -->\n\n## Description\n\nDescribe what went wrong and how often it happens.\n\n'
            '## Steps to reproduce\n\n1.\n2.\n3.\n\nMap name/ID or story progress, if known:\n\n'
            '## Expected and actual behavior\n\n- Expected:\n- Actual:\n')


def issue(number, title, body='', created='2026-09-01T00:00:00Z', **extra):
    return dict({'number': number, 'title': title, 'body': body, 'created_at': created,
                 'state': 'closed', 'labels': [{'name': 'bug'}]}, **extra)


class RelatedTests(unittest.TestCase):
    def test_clean_body_drops_template_keeps_text(self):
        text = TEMPLATE.replace('Describe what went wrong and how often it happens.',
                                'Battle music keeps playing after the fight.')
        self.assertEqual(related.clean_body(text), 'Battle music keeps playing after the fight.')

    def test_synonyms_match_and_unrelated_does_not(self):
        target = issue(1, '[Bug] Music issue', 'battle bgm keeps playing after the fanfare', '2026-10-14T00:00:00Z')
        pool = [issue(2, '[Bug] Missing audio', 'cutscene has no sound'),
                issue(3, '[Feature] 32:9', 'ultrawide support for the aspect ratio')]
        picked = related.select(target, [target] + pool, NOW)
        self.assertEqual(picked[0]['number'], 2)
        self.assertNotIn(1, [c['number'] for c in picked])
        self.assertNotIn(3, [c['number'] for c in picked])

    def test_recent_issues_always_included_and_prs_excluded(self):
        target = issue(1, 'Crash on load', 'it crashes')
        recent = issue(2, 'Unrelated topic', 'zzz', '2026-10-13T00:00:00Z')
        old = issue(3, 'Another unrelated thing', 'qqq', '2026-08-01T00:00:00Z')
        pr = issue(4, 'Crash on load fix', 'crash', '2026-10-13T00:00:00Z', pull_request={})
        numbers = [c['number'] for c in related.select(target, [target, recent, old, pr], NOW)]
        self.assertEqual(numbers, [2])

    def test_block_is_capped(self):
        target = issue(1, 'music', 'music')
        pool = [issue(n, f'music {n}', 'music ' * 200, '2026-10-13T00:00:00Z') for n in range(2, 200)]
        picked = related.select(target, pool, NOW)
        self.assertLessEqual(sum(len(str(c)) for c in picked), related.BLOCK_LIMIT)
        self.assertTrue(all(len(c['excerpt']) <= related.EXCERPT for c in picked))

    def test_unknown_issue_references_are_removed(self):
        answer = 'Fine.\nPossibly related: #5 (same), #9 (other)\nSee #5.'
        self.assertEqual(related.strip_unknown_refs(answer, {5}), 'Fine.\nSee #5.')


if __name__ == '__main__':
    unittest.main()
