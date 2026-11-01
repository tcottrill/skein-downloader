# Translations

Use the **flag and current-language button** in the top toolbar and restart Skein Downloader. The default is **Same
as system**. English, Simplified and Traditional Chinese, Russian, Japanese, Korean, French, Italian, Spanish, German, Vietnamese, Portuguese and Hindi
are available. Language names are shown in their own languages so the selector
remains recognizable after an accidental selection. The dropdown pairs each
language with a round flag icon; **Same as system** uses a globe. The checked-in
PNG sizes are embedded into the executable and cached when the selector is created.
The button shows the language currently in use, such as **English**, even when
**Same as system** is selected. A new choice applies after restarting.

System preferences such as `fr-CA` and `ja-JP` resolve to the available base
language. Spanish (`Español`) uses a shared initial catalog for Spain and Latin
America, including `es-ES`, `es-MX` and `es-419`, with a round Spain flag.
Korean (`한국어`) supports `ko-KR` and uses a round South Korean flag.
Simplified Chinese supports `zh-CN`, `zh-SG`, and `zh-Hans` variants.
Traditional Chinese (`繁體中文`) supports `zh-TW`, `zh-HK`, `zh-MO`, and `zh-Hant` variants, using a Taiwan flag.
German (`Deutsch`) and Vietnamese (`Tiếng Việt`) support regional variants such as `de-AT` and `vi-VN`, with Germany and Vietnam flags. Portuguese (`Português`) uses one initial catalog for Portugal and Brazil (`pt-PT` and `pt-BR`), with a Portugal flag. Hindi (`हिन्दी`) supports `hi-IN` with an India flag.
Unsupported languages and untranslated messages fall back to English.
On Windows, if no supported system UI language is available, ANSI code pages
932, 936, 949, 950, 1251 and 1258 select Japanese, Simplified Chinese, Korean, Traditional Chinese, Russian and Vietnamese respectively. Portuguese and Hindi use OS language preferences; Western code page 1252 and UTF-8 do not distinguish them.
Shared code pages such as 1252 and UTF-8 need system-language information to
distinguish languages; without it they fall back to English. Explicit choices
override automatic detection for every supported language, not just English.
The selection is saved immediately and retained across restarts. Only **Same as
system** enables automatic language/code-page detection. The app does not change
the Windows code page.

## Editing translations

The UTF-8 `.po` files are standard GNU gettext catalogs, editable with Poedit or
a text editor. Initial application translations are a starting point for native
speaker review. Preserve printf arguments (`%s`, `%zu`, `%d`), newlines, leading
and trailing spaces, and menu shortcuts. `&` marks a menu access key and `\tDel`
marks the Delete shortcut. Translate full sentences rather than fragments when
adding new messages.

After editing, run from the repository root:

```console
python tools/translations.py
python tools/test_translations.py
python tools/translations.py --check
```

Commit both the source `.po` files and generated `.mo` files. CMake embeds the
compiled catalogs in the executable, including the matching wxWidgets standard
catalogs from `vendor/wxWidgets/locale`. Normal builds and packaging need neither
Python nor gettext nor external translation files. Updating wxWidgets may require
regenerating its compiled catalogs. Upstream wxWidgets catalogs retain their
original licenses and translator credits in the vendored source files.

## Marking new messages

Use `_(L"English text")` for UI literals. The wide literal is intentional: it
keeps punctuation such as ellipses independent of the Windows ANSI code page.
Use `wxString::Format(_(L"Message with %d values"), count)` for substituted values.
Use `lite::translated()` at the display boundary for existing UTF-8 backend
messages, and mark those English values with `wxTRANSLATE()` in
`src/wx/localization.cpp`. Never translate settings keys, queue states in storage,
URLs, filenames, JSON fields, browser identifiers, or external-tool arguments.

Run `python tools/translations.py --update` to refresh the template and catalogs.
This refreshes the currently used singular message IDs, preserves existing
translations and fuzzy status, and creates empty entries for new messages. The
command reports incomplete translations until they are filled in. The compiler
also understands standard gettext plural and context entries; the current source
extractor handles `_()` and `wxTRANSLATE()` singular literals. Use gettext tooling
if introducing plural or contextual message calls, and extend the extractor
before relying on `--update` for them.

To add a language, add its code and native display name in
`src/wx/localization.h`, its resolution rule in `localization.cpp`, and its code
to `tools/translations.py` and the catalog loop in `CMakeLists.txt`. Supply its
application PO file and a matching wxWidgets catalog. Then compile and test.
If the upstream wxWidgets catalog uses a regional filename, add its mapping to
`WX_CATALOG_NAMES` in `tools/translations.py` (Korean uses `ko_KR.po`).
Also add the language to the Windows installer, as described below.

## Windows installer

The Inno Setup installer uses the same languages. It follows the Windows display
language and shows Inno's language picker only when that language is not one of
them. Its wizard text comes from Inno Setup's bundled translations, or from
`packaging/windows/languages` for the four Inno Setup does not ship (see the README
there; Hindi's predates Inno Setup 6, so newer wizard messages appear in English).
The installer's own text (the download page, progress and errors) is in
`packaging/windows/messages/<code>.isl`, one file per app language with the same
names as `en.isl`. `%1` and `%2` are filled in by the script and `%n` starts a new
line. When a message mentions the Install button, use that language's label for it.

To add a language, add its `messages/<code>.isl` and a `[Languages]` entry in
`packaging/windows/skein-downloader.iss` that lists Inno's messages file followed by
it. `python tools/translations.py` (and its CTest) checks that every app language
has the installer's messages, with the same arguments as English.

## Scope and validation

Application controls, menus, hints, common statuses, and wxWidgets catalog text
are translated. Filenames, website-provided metadata, raw yt-dlp/installer output,
technical backend diagnostics without catalog entries, and the license notice
remain in their original language. OS-owned dialogs may follow the OS language.
Translations do not change the process's numeric locale or saved queue data.

```console
cmake --build --preset vs2022-release
ctest --test-dir build/vs2022 -C Release --output-on-failure
python tools/test-localization-smoke.py dist/skein-downloader.exe --screenshots
```

The native smoke test launches real windows with disposable profiles, checking
queue selection/filtering, tabs, file selection, video controls and shutdown in
each supported language and an unsupported-language fallback.
