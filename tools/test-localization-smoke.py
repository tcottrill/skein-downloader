"""Exercise real native windows in each language using disposable profiles.

Usage: python tools/test-localization-smoke.py dist/skein-downloader.exe
Add --screenshots to retain main/video screenshots in build/localization-smoke/.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--screenshots', action='store_true')
    parser.add_argument('--language', choices=('en', 'zh_CN', 'ru', 'ja', 'fr', 'it', 'es', 'ko', 'de', 'zh_TW', 'vi', 'pt', 'hi', 'unsupported'))
    parser.add_argument('--width', type=int, default=1100, help='Test main-window width in logical pixels')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / 'build/localization-smoke'
    output.mkdir(parents=True, exist_ok=True)
    executable = args.executable.resolve()
    for language in ([args.language] if args.language else ('en', 'zh_CN', 'ru', 'ja', 'fr', 'it', 'es', 'ko', 'de', 'zh_TW', 'vi', 'pt', 'hi', 'unsupported')):
        for video in ((False, True) if args.screenshots else (False,)):
            with tempfile.TemporaryDirectory(prefix=language+'-', dir=output) as temporary:
                profile = Path(temporary)
                (profile / 'settings.ini').write_text(f'language={language}\nwindowWidth={args.width}\n', encoding='utf-8')
                command = [str(executable), '--smoke-test', '--data-dir', str(profile)]
                if video:
                    command.append('--video')
                if args.screenshots:
                    image = output / (language + ('-video' if video else '-main') + (f'-{args.width}' if args.width != 1100 else '') + ('.bmp' if os.name == 'nt' else '.png'))
                    command += ['--screenshot', str(image)]
                result = subprocess.run(command, timeout=30, capture_output=True)
                log = (profile/'skein-downloader.log').read_text(encoding='utf-8', errors='replace') if (profile/'skein-downloader.log').exists() else ''
                (output / (language + ('-video' if video else '-main') + '.log')).write_text(log, encoding='utf-8')
                if result.returncode or not (profile/'smoke-passed.txt').exists():
                    raise RuntimeError(f'{language}: smoke failed ({result.returncode})\n{log}\n{result.stderr!r}')
                settings = (profile/'settings.ini').read_text(encoding='utf-8')
                if 'language='+language not in settings:
                    raise AssertionError('Saved language changed unexpectedly')
                # Smoke exercises queue insertion, filtering, tabs, file selection,
                # video options, and shutdown. Verify the persisted JSON still parses.
                json.loads((profile/'history.json').read_text(encoding='utf-8'))
                print(language + (' video' if video else ' main') + ': passed', flush=True)


if __name__ == '__main__':
    main()
