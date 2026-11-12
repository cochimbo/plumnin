# Translations

Texts are written in Spanish in the code, and each language has its `.po` here
(`en.po`, `fr.po`, `pt.po`), which is built into the executable.

## When adding or changing a text

1. Wrap it in the code:
   - `tr("Guardar estado")` for a text shown as is (`"Text##id"` works too: only what
     comes before `##` is translated).
   - `trf("Ranura %d", n)` when it has `%d`, `%s`...
   - `N_("...")` in tables or constants that are translated later with `tr(...)`.
2. Run `python3 tools/i18n.py --update`: it adds the new texts to the `.po` files (with an
   empty translation) and removes the ones that are gone.
3. Fill in the `msgstr` in all three `.po` files (`python3 tools/i18n.py --missing` lists
   what is missing). Keep the `%d`/`%s` in the same order.
4. `python3 tools/i18n.py --check` (the `i18n` test in `ctest`) fails while anything is
   missing.

The `.po` files are kept sorted, so two branches that add different texts rarely clash.

## Adding a language

Add its code to `PLUMNIN_LANGUAGES` (CMakeLists.txt), to `LANGUAGES` (tools/i18n.py) and
to `kLanguages` (src/frontend/languages.cpp) with its name, and create the `.po` with
`tools/i18n.py --update`.
