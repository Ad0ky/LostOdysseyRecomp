"""Download attached runtime logs and reduce them to a small deterministic digest (no model involved)."""
import io
import re
import urllib.error
import urllib.parse
import urllib.request
import zipfile

MAX_DOWNLOAD = 2 * 1024 * 1024
MAX_ZIP = 25 * 1024 * 1024
MAX_MEMBER = 20 * 1024 * 1024
MAX_RATIO = 100
MAX_UNZIPPED = 60 * 1024 * 1024
MAX_MEMBERS = 500
ZIP_LOGS = 2
PER_LOG = 12 * 1024
TOTAL = 24 * 1024
MAX_LOGS = 3
ATTACHMENT = re.compile(r'https://github\.com/user-attachments/files/\d+/[^\s)\]>"\'<]+?\.(?:log|txt|zip)(?![\w.])', re.I)
UNSUPPORTED = re.compile(r'https://github\.com/user-attachments/files/\d+/[^\s)\]>"\'<]+?\.(?:7z|rar)(?![\w.])', re.I)
ALLOWED_HOSTS = ('github.com', 'githubusercontent.com')
LINE = re.compile(r'^\[\s*[\d.]+ \w+\] \[(\w+)\]\s+(.*)$')
HEADER = re.compile(r'(source version|build:|host OS|host architecture|mods:|game edition|settings:|'
                    r'LO_\* switches|video device:|video: \w+ on )', re.I)
LEVELS = ('warn', 'warning', 'error', 'fatal', 'crit', 'critical')


class LogError(Exception):
    pass


class SameFamilyRedirect(urllib.request.HTTPRedirectHandler):
    """Attachments redirect to GitHub's object storage; follow only https redirects inside GitHub hosts."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parts = urllib.parse.urlsplit(newurl)
        host = parts.hostname or ''
        if parts.scheme != 'https' or not any(host == h or host.endswith('.' + h) for h in ALLOWED_HOSTS):
            raise LogError('Redirect refused')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def attachments(text):
    """Distinct .log/.txt attachment URLs in the text, newest runtime log first."""
    urls = list(dict.fromkeys(ATTACHMENT.findall(str(text or ''))))
    def stamp(url):
        m = re.search(r'runtime-(\d+)', url)
        return int(m.group(1)) if m else 0
    return sorted(urls, key=stamp, reverse=True)[:MAX_LOGS]


def unsupported(text):
    return bool(UNSUPPORTED.search(str(text or '')))


def download(url, timeout=30, limit=MAX_DOWNLOAD):
    # No credentials are sent; a file over the size cap is skipped rather than cut, since the tail matters.
    req = urllib.request.Request(url, headers={'User-Agent': 'lost-odyssey-issue-triage'})
    try:
        with urllib.request.build_opener(SameFamilyRedirect).open(req, timeout=timeout) as response:
            raw = response.read(limit + 1)
    except (urllib.error.URLError, OSError, ValueError, LogError):
        raise LogError('Log download failed') from None
    if len(raw) > limit:
        raise LogError('Download exceeds size limit')
    return raw.decode('utf-8', errors='replace')


def _normalize(message):
    return re.sub(r'0x[0-9a-fA-F]+|\d+(?:\.\d+)?', 'N', message)


def _cut(line, size):
    return line if len(line) <= size else line[:size - 3] + '...'


def digest(text, issues=60, tail=60, cut=220):
    lines = text.splitlines()
    header = []
    for line in lines[:120]:
        m = LINE.match(line)
        if m and m.group(1).lower() not in LEVELS and HEADER.match(m.group(2)):
            header.append(_cut(m.group(2), 400))
    groups = {}
    for line in lines:
        m = LINE.match(line)
        if m and m.group(1).lower() in LEVELS:
            key = (m.group(1).lower(), _normalize(m.group(2)))
            if key in groups:
                groups[key][0] += 1
            else:
                groups[key] = [1, _cut(line, cut)]
    shown = list(groups.values())[:issues]
    out = ['Header facts:'] + (header or ['(none found)'])
    out.append(f'Warnings and errors (deduplicated, {len(groups)} distinct):')
    out += [f'x{count} {example}' for count, example in shown] or ['(none)']
    out.append(f'Last {min(tail, len(lines))} lines:')
    out += [_cut(l, cut) for l in lines[-tail:]]
    return '\n'.join(out)


def fit(text, limit=PER_LOG):
    for issues, tail, cut in ((60, 60, 220), (40, 40, 180), (25, 25, 140), (15, 15, 120), (8, 8, 100)):
        result = digest(text, issues, tail, cut)
        if len(result) <= limit:
            return result
    return result[:limit]


def zip_logs(raw):
    """(member name, text) for the newest runtime logs in a zip, read in memory only."""
    try:
        archive = zipfile.ZipFile(io.BytesIO(raw))
        infos = archive.infolist()[:MAX_MEMBERS]
    except (zipfile.BadZipFile, OSError, ValueError, NotImplementedError):
        raise LogError('Unreadable zip') from None
    usable = []
    for info in infos:
        base = info.filename.replace(chr(92), '/').rsplit('/', 1)[-1]
        if (info.is_dir() or info.flag_bits & 0x1 or info.file_size > MAX_MEMBER
                or info.file_size > MAX_RATIO * max(info.compress_size, 1)
                or not re.search(r'\.(?:log|txt)$', base, re.I)):
            continue
        usable.append((info, base))
    def stamp(item):
        m = re.fullmatch(r'runtime-(\d+)\.log', item[1], re.I)
        return (1, int(m.group(1)), item[0].date_time) if m else (0, 0, item[0].date_time)
    runtime = [u for u in usable if stamp(u)[0]]
    chosen = sorted(runtime or usable, key=stamp, reverse=True)[:ZIP_LOGS]
    result, read = [], 0
    for info, _ in chosen:
        if read + info.file_size > MAX_UNZIPPED:
            break
        try:
            with archive.open(info) as member:
                data = member.read(MAX_MEMBER + 1)
        except (zipfile.BadZipFile, RuntimeError, OSError, ValueError, NotImplementedError, EOFError):
            continue
        read += len(data)
        if len(data) <= MAX_MEMBER:
            result.append((info.filename, data.decode('utf-8', errors='replace')))
    return result


def collect(body, fetch=download):
    """Digests for the attached logs and zips, newest first, within the total budget. Failures are skipped."""
    sources, used, result = [], 0, []
    for url in attachments(body):
        name = urllib.parse.unquote(url.rsplit('/', 1)[-1])[:80]
        try:
            if url.lower().endswith('.zip'):
                sources += [(f'{name}/{member}'[:120], text) for member, text in zip_logs(fetch(url, limit=MAX_ZIP))]
            else:
                sources.append((name, fetch(url)))
        except LogError:
            continue
    for name, text in sources[:MAX_LOGS]:
        if TOTAL - used < 1000:
            break
        item = fit(text, min(PER_LOG, TOTAL - used))
        result.append({'file': name, 'digest': item})
        used += len(item)
    return result
