"""Behavior tests for the catalog compiler (no third-party Python packages)."""
import gettext
import importlib.util
import io
from pathlib import Path
import unittest

SCRIPT = Path(__file__).with_name('translations.py')


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), 'Translation compiler is not implemented')
        spec = importlib.util.spec_from_file_location('translations', SCRIPT)
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)

    def test_unicode_plural_fuzzy_and_fallback(self):
        po = '''msgid ""
msgstr "Content-Type: text/plain; charset=UTF-8\\nPlural-Forms: nplurals=2; plural=(n != 1);\\n"

msgid "Language"
msgstr "日本語"

msgid "file"
msgid_plural "files"
msgstr[0] "fichier"
msgstr[1] "fichiers"

#, fuzzy
msgid "Unreviewed"
msgstr "Ignore me"

msgid "Empty"
msgstr ""
'''
        catalog = gettext.GNUTranslations(io.BytesIO(self.tool.compile_po(po)))
        self.assertEqual(catalog.gettext('Language'), '日本語')
        self.assertEqual(catalog.ngettext('file', 'files', 2), 'fichiers')
        self.assertEqual(catalog.ngettext('file', 'files', 1), 'fichier')
        self.assertEqual(catalog.gettext('Unreviewed'), 'Unreviewed')
        self.assertEqual(catalog.gettext('Empty'), 'Empty')
        self.assertEqual(catalog.gettext('Missing'), 'Missing')

    def test_rejects_changed_format_arguments(self):
        po = 'msgid "Added %zu files"\nmsgstr "Ajouté %s fichiers"\n'
        with self.assertRaisesRegex(ValueError, 'format'):
            self.tool.compile_po(po)

    def test_rejects_duplicate_messages(self):
        po = 'msgid "A"\nmsgstr "B"\n\nmsgid "A"\nmsgstr "C"\n'
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.tool.compile_po(po)

    def test_rejects_write_count_format(self):
        with self.assertRaisesRegex(ValueError, 'format'):
            self.tool.compile_po('msgid "Hello"\nmsgstr "Hello %n"\n')

    def test_rejects_hidden_positional_format(self):
        with self.assertRaisesRegex(ValueError, 'format'):
            self.tool.compile_po('msgid "%s"\nmsgstr "%s %2$n"\n')

    def test_update_preserves_fuzzy_review_status(self):
        old = {'Hello': {'msgid': 'Hello', 'msgstr': 'Bonjour', 'fuzzy': True}}
        updated = self.tool.po_text(['Hello'], old, 'fr')
        catalog = gettext.GNUTranslations(io.BytesIO(self.tool.compile_po(updated)))
        self.assertEqual(catalog.gettext('Hello'), 'Hello')


if __name__ == '__main__':
    unittest.main()
