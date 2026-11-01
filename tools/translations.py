"""Update gettext sources and compile embedded catalogs using Python 3 only.

Run with --update after marking new strings with _() or wxTRANSLATE().
Run without options after editing PO files. --check verifies checked-in outputs.
Every run also checks the installer's text in packaging/windows/messages.
"""
import argparse
import ast
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
LANGUAGES = ('zh_CN', 'ru', 'ja', 'fr', 'it', 'es', 'ko', 'de', 'zh_TW', 'vi', 'pt', 'hi')
WX_CATALOG_NAMES = {'ko': 'ko_KR'}
QUOTED = r'"(?:[^"\\]|\\.)*"'


def read_po(text):
    entries, entry, field = [], {}, None
    for line in text.splitlines() + ['']:
        line = line.strip()
        if not line:
            if 'msgid' in entry:
                entries.append(entry)
            entry, field = {}, None
        elif line.startswith('#'):
            if line.startswith('#,') and 'fuzzy' in line:
                entry['fuzzy'] = True
        elif line.startswith('"'):
            if field is None:
                raise ValueError('Unexpected PO continuation')
            entry[field] += ast.literal_eval(line)
        else:
            match = re.fullmatch(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(' + QUOTED + ')', line)
            if not match:
                raise ValueError('Invalid PO line: ' + line)
            field = match[1]
            if field == 'msgid' and field in entry:
                entries.append(entry)
                entry = {}
            entry[field] = ast.literal_eval(match[2])
    return entries


def formats(text):
    # Preserve argument types and order; translations may change surrounding prose.
    directive = re.compile(r'%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcsp])')
    result, position = [], 0
    while (position := text.find('%', position)) != -1:
        match = directive.match(text, position)
        if not match:
            raise ValueError('Unsupported format directive: ' + text[position:])
        if match[0] != '%%':
            result.append(match[0])
        position = match.end()
    return result


def compile_po(text, validate=True):
    messages, seen = {}, set()
    for e in read_po(text):
        key = (e.get('msgctxt', '') + '\x04' if 'msgctxt' in e else '') + e['msgid']
        if key in seen:
            raise ValueError('Duplicate message: ' + key)
        seen.add(key)
        if e.get('fuzzy'):
            continue
        if 'msgid_plural' in e:
            key += '\0' + e['msgid_plural']
            values = [e[k] for k in sorted((k for k in e if k.startswith('msgstr[')), key=lambda k: int(k[7:-1]))]
        else:
            values = [e.get('msgstr', '')]
        if not values or not all(values):
            continue
        if validate and e['msgid']:
            for value in values:
                if formats(e['msgid']) != formats(value):
                    raise ValueError('Changed format arguments: ' + e['msgid'])
        messages[key.encode('utf-8')] = '\0'.join(values).encode('utf-8')
    keys = sorted(messages)
    count = len(keys)
    offset = 28 + count * 16
    originals, translations, data = [], [], bytearray()
    for key in keys:
        originals.append((len(key), offset + len(data)))
        data += key + b'\0'
    for key in keys:
        value = messages[key]
        translations.append((len(value), offset + len(data)))
        data += value + b'\0'
    return (struct.pack('<7I', 0x950412de, 0, count, 28, 28 + count * 8, 0, 0)
            + b''.join(struct.pack('<2I', *row) for row in originals + translations) + data)


def source_messages():
    messages = set()
    pattern = re.compile(r'\b(?:_|wxTRANSLATE)\(\s*((?:L?' + QUOTED + r'\s*)+)\)')
    for path in (ROOT / 'src/wx').glob('*'):
        if path.suffix in ('.cpp', '.h'):
            source = path.read_text(encoding='utf-8')
            for match in pattern.finditer(source):
                messages.add(''.join(ast.literal_eval(s) for s in re.findall(QUOTED, match[1])))
    return sorted(messages)


def po_text(messages, existing, language):
    entries = {key: value if isinstance(value, dict) else {'msgstr': value} for key, value in existing.items()}
    existing = {key: value.get('msgstr', '') for key, value in entries.items()}
    header = existing.get('', 'Project-Id-Version: Skein Downloader\nLanguage: ' + language + '\nContent-Type: text/plain; charset=UTF-8\nPlural-Forms: nplurals=2; plural=(n != 1);\n')
    quote = lambda s: json.dumps(s, ensure_ascii=False)
    result = '# Skein Downloader translations. GPL-3.0-or-later.\nmsgid ""\nmsgstr ' + quote(header) + '\n\n'
    for message in messages:
        if entries.get(message, {}).get('fuzzy'):
            result += '#, fuzzy\n'
        if formats(message):
            result += '#, c-format\n'
        result += 'msgid ' + quote(message) + '\nmsgstr ' + quote(existing.get(message, '')) + '\n\n'
    return result


def installer_messages(path):
    messages, section = {}, None
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line.startswith('['):
            section = line
        elif line and not line.startswith(';') and section == '[CustomMessages]':
            name, _, value = line.partition('=')
            messages[name] = value
    return messages


def check_installer():
    # The installer's own text (packaging/windows/messages) in every app language, with the
    # names the script uses and the same %1... arguments as English.
    folder = ROOT / 'packaging/windows'
    script = (folder / 'skein-downloader.iss').read_text(encoding='utf-8')
    english = installer_messages(folder / 'messages/en.isl')
    unknown = set(re.findall(r"\bCustomMessage\('(\w+)'", script)) - set(english)
    if unknown:
        raise ValueError(f'Installer messages missing from en.isl: {sorted(unknown)}')
    for language in ('en',) + LANGUAGES:
        if f'messages\\{language}.isl' not in script:
            raise ValueError(f'{language}: not in the installer [Languages] section')
        messages = installer_messages(folder / 'messages' / (language + '.isl'))
        if set(messages) != set(english):
            raise ValueError(f'{language}: installer messages differ from en.isl: {sorted(set(messages) ^ set(english))}')
        for name, value in messages.items():
            if sorted(re.findall(r'%\d', value)) != sorted(re.findall(r'%\d', english[name])):
                raise ValueError(f'{language}: changed arguments in installer message {name}')
    print(f'Installer: {len(english)} messages in {len(LANGUAGES) + 1} languages')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    folder = ROOT / 'locale'
    folder.mkdir(exist_ok=True)
    messages = source_messages()
    if args.update:
        (folder / 'skein.pot').write_text(po_text(messages, {}, ''), encoding='utf-8')
        for language in LANGUAGES:
            path = folder / (language + '.po')
            old = {e['msgid']: e for e in read_po(path.read_text(encoding='utf-8'))} if path.exists() else {}
            path.write_text(po_text(messages, old, language), encoding='utf-8')
    for language in LANGUAGES:
        path = folder / (language + '.po')
        content = path.read_text(encoding='utf-8')
        translated = {e['msgid'] for e in read_po(content) if e.get('msgstr') and not e.get('fuzzy')}
        missing = set(messages) - translated
        if missing:
            raise ValueError(f'{language}: {len(missing)} untranslated messages: {sorted(missing)}')
        wx_language = WX_CATALOG_NAMES.get(language, language)
        for domain, source in [('skein', content), ('wxstd', (ROOT / 'vendor/wxWidgets/locale' / (wx_language + '.po')).read_text(encoding='utf-8-sig'))]:
            data = compile_po(source, validate=domain == 'skein')
            output = folder / (language + '.' + domain + '.mo')
            if args.check:
                if not output.exists() or output.read_bytes() != data:
                    raise ValueError('Stale compiled catalog: ' + str(output))
            else:
                output.write_bytes(data)
        print(f'{language}: {len(messages)} application messages, wxWidgets catalog compiled')
    check_installer()


if __name__ == '__main__':
    main()
