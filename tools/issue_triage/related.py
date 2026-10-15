"""Candidate issues that may relate to a new report, gathered locally before the tool-less model call."""
from datetime import datetime, timedelta, timezone
import json
import math
import re

RECENT_DAYS = 14
TOP_SCORED = 12
EXCERPT = 400
BLOCK_LIMIT = 12 * 1024
PAGE = 50
MAX_PAGES = 30

# Words that should match each other; keep this list short.
SYNONYMS = {}
for _group in ('music bgm fanfare audio sound sounds voice song soundtrack sfx',
               'crash crashes crashed crashing freeze freezes froze hang hangs',
               'cutscene cutscenes cinematic movie fmv video',
               'controller gamepad joystick'):
    _words = _group.split()
    SYNONYMS.update({w: _words[0] for w in _words})

STOPWORDS = set('''the and for with that this when then than from have has had was were are but not
into onto over under after before during while there here they them their its out can could would
should will just still also only very some any all one two get got does did doing been being bug
issue game games play playing played map name story progress known'''.split())

# Lines of the issue templates; a line equal to one of these carries no information.
BOILERPLATE = {s.lower() for s in (
    'Describe what went wrong and how often it happens.',
    'If no log was created, say so here.',
    'Map name/ID or story progress, if known:',
    'Logs are not required for feature requests.',
    'What are you trying to do, and what is difficult or missing today?',
    'Describe the feature and how you would expect it to work.',
)}
BOILERPLATE_PREFIXES = ('attach the latest log from the affected run',)


def clean_body(text):
    """Drop template headings, comments, placeholders and attachment links; keep what the reporter wrote."""
    text = re.sub(r'<!--.*?-->', ' ', str(text or ''), flags=re.S)
    text = re.sub(r'!?\[([^\]]*)\]\(https?://[^)]*\)', lambda m: ' ' if re.search(r'\.(log|zip|png|jpe?g|mp4|txt)$|^##$', m.group(1), re.I) else m.group(1), text)
    text = re.sub(r'https?://\S+', ' ', text)
    lines = []
    for raw in text.splitlines():
        line = raw.strip()
        low = line.lower()
        if (not line or line.startswith('#') or line == '---' or low in BOILERPLATE
                or low.startswith(BOILERPLATE_PREFIXES) or low == '_no response_'
                or re.fullmatch(r'(?:[-*]|\d+\.)?\s*(?:expected|actual)?\s*:?', low)):
            continue
        lines.append(line)
    return re.sub(r'\s+', ' ', ' '.join(lines)).strip()


def tokens(text):
    """Lowercase word tokens with synonyms folded; CJK text becomes character bigrams."""
    text = str(text or '').lower()
    result = [SYNONYMS.get(w, w) for w in re.findall(r'[a-z0-9]{3,}', text) if w not in STOPWORDS]
    for run in re.findall(r'[぀-ヿ㐀-鿿]+', text):
        result.extend(run[i:i + 2] for i in range(max(len(run) - 1, 1)))
    return result


def score_all(target, pool):
    """Rank pool items against target; each is {'title','clean'}. Returns a score per pool item."""
    docs = [set(tokens(p['title'] + ' ' + p['clean'])) for p in pool]
    titles = [set(tokens(p['title'])) for p in pool]
    df = {}
    for doc in docs:
        for t in doc:
            df[t] = df.get(t, 0) + 1
    n = len(pool) + 1
    idf = lambda t: math.log(1 + n / (1 + df.get(t, 0)))
    want = set(tokens(target['title'] + ' ' + target['clean']))
    want_title = set(tokens(target['title']))
    scores = []
    for doc, title in zip(docs, titles):
        shared = sum(idf(t) for t in want & doc)
        shared += sum(idf(t) for t in want_title & title)  # a title match counts twice
        scores.append(shared / math.sqrt(len(doc) + 20))
    return scores


def entry(issue, clean):
    return {'number': issue['number'], 'title': str(issue.get('title') or '')[:150],
            'state': issue.get('state'),
            'labels': [str(l.get('name'))[:40] for l in issue.get('labels', []) if isinstance(l, dict)][:6],
            'created': str(issue.get('created_at') or '')[:10], 'excerpt': clean[:EXCERPT]}


def select(target_issue, issues, now=None):
    """Pick candidate entries: top keyword matches first, then every recent issue, within BLOCK_LIMIT."""
    now = now or datetime.now(timezone.utc)
    number = target_issue.get('number')
    pool = [i for i in issues if 'pull_request' not in i and i.get('number') != number]
    cleaned = [clean_body(i.get('body')) for i in pool]
    infos = [{'title': str(i.get('title') or ''), 'clean': c} for i, c in zip(pool, cleaned)]
    target = {'title': str(target_issue.get('title') or ''), 'clean': clean_body(target_issue.get('body'))}
    scores = score_all(target, infos)
    order = sorted(range(len(pool)), key=lambda k: -scores[k])
    chosen = [k for k in order if scores[k] > 0][:TOP_SCORED]
    cutoff = now - timedelta(days=RECENT_DAYS)
    recent = []
    for k, issue in enumerate(pool):
        try:
            created = datetime.fromisoformat(str(issue.get('created_at')).replace('Z', '+00:00'))
        except ValueError:
            continue
        if created >= cutoff and k not in chosen:
            recent.append((created, k))
    chosen += [k for _, k in sorted(recent, reverse=True)]
    result, size = [], 2
    for k in chosen:
        item = entry(pool[k], cleaned[k])
        item_size = len(json.dumps(item, ensure_ascii=False)) + 2
        if size + item_size > BLOCK_LIMIT:
            continue
        result.append(item)
        size += item_size
    return result


def list_issues(fetch, issues_url):
    """All issues (open and closed, PRs included in the API list) newest first; bounded pagination."""
    issues = []
    for page in range(1, MAX_PAGES + 1):
        batch = fetch(f'{issues_url}?state=all&sort=created&direction=desc&per_page={PAGE}&page={page}')
        if not isinstance(batch, list):
            raise ValueError('Invalid issue list')
        issues.extend(i for i in batch if isinstance(i, dict) and 'pull_request' not in i)
        if len(batch) < PAGE:
            break
    return issues


def candidates(fetch, issues_url, issue, now=None):
    return select(issue, list_issues(fetch, issues_url), now)


def strip_unknown_refs(answer, allowed):
    """Remove lines that cite an issue number the model was not given (no invented links)."""
    keep = []
    for line in answer.split('\n'):
        refs = {int(n) for n in re.findall(r'(?<![\w&])#([0-9]+)\b', line)}
        if refs - allowed:
            continue
        keep.append(line)
    return '\n'.join(keep).strip()
