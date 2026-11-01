# Localization implementation plan

Goal: add system-language detection and a saved, restart-based language selector,
with initial Simplified Chinese, Russian, Japanese, French and Italian translations.
The user authorized implementation with judgment on remaining details, then
requested a dedicated top-toolbar language button with a dropdown containing
round country icons to the left of the native language names (not in About).

Architecture: wxTranslations with embedded GNU gettext catalogs. Only displayed
text is translated; queue identities, protocol strings, settings keys and log
output retain their existing representation. English is the fallback. Catalogs
are embedded to cover all existing package formats without deployment changes.

- [x] Add catalog compiler and tests for Unicode, format arguments, missing entries,
  fuzzy entries and plural forms. Keep compiled catalogs checked in so normal builds
  need no additional tools.
- [x] Add embedded catalog loader, system-language resolution and saved selection.
  Test unsupported locales, regional variants, explicit English and translations.
- [x] Mark user-interface text and translate the five catalogs, including standard
  wxWidgets controls. Preserve backend state comparisons and persisted values.
- [ ] Build and exercise the existing smoke path in every language. Check layout
  and document catalog maintenance and the limits of translating external output.

Verification before toolbar revision: Windows Release build, three CTest suites,
and fourteen native UI smoke runs passed. Linux configure in WSL is blocked by
missing GTK3 development files. Independent review identified a format-validation
gap for positional directives; a failing regression test reproduced it and the
compiler now rejects unsupported directives. Six compiler tests pass.

Review focus: non-ASCII strings, printf type safety, unsupported Chinese variants,
missing catalog fallback, and labels longer than their English counterparts.

Execution: in the user's existing checkout, retaining its configured builds.
