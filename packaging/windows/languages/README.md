# Inno Setup translations not bundled with Inno Setup

The installer uses Inno Setup's bundled translations for English, French, German,
Italian, Japanese, Korean, Portuguese, Russian and Spanish. These four files cover
the other app languages. They are unmodified copies from Inno Setup's repository,
`jrsoftware/issrc` at commit `5bcadfcd05e9857a47a6880a2ac0694c9ab60122`
(downloaded 2026-11-01), and keep their translators' credits in their headers.

| File | Source path | Status on jrsoftware.org/files/istrans |
| --- | --- | --- |
| `ChineseSimplified.isl` | `Files/Languages/ChineseSimplified.isl` | Official, Inno Setup 6.5.0+ |
| `ChineseTraditional.isl` | `Files/Languages/ChineseTraditional.isl` | Official, Inno Setup 6.5.0+ |
| `Vietnamese.isl` | `Files/Languages/Unofficial/Vietnamese.isl` | Unofficial, Inno Setup 6.5.0+ |
| `Hindi.islu` | `Files/Languages/Unofficial/Hindi.islu` | Unofficial, Inno Setup 5.5.3+ |

`Hindi.islu` predates Inno Setup 6, so the wizard text added since then appears in
English (the compiler warns about the missing messages). When updating Inno Setup,
replace these files with newer copies from the same paths, or drop one once Inno
Setup bundles it and use `compiler:Languages\...` in `skein-downloader.iss` instead.

The installer's own text (the download page, progress and errors) is in
`../messages`, one file per app language.
