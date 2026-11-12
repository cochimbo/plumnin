#!/usr/bin/env python3
"""Keeps the translations in src/frontend/lang/*.po in step with the code.

The texts are written in Spanish in the code, inside tr("..."), trf("...", ...) or
N_("..."). Each .po has one entry per text, sorted, with its translation in msgstr.

  tools/i18n.py --update   adds the new texts (with an empty msgstr) and drops the gone ones
  tools/i18n.py --check    fails if a text is missing, untranslated, or its %-specifiers
                           differ from the Spanish (what CI runs, as the ctest "i18n")
  tools/i18n.py --missing  prints, per language, the texts still to translate
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = [os.path.join(ROOT, "src")]
LANG_DIR = os.path.join(ROOT, "src", "frontend", "lang")
LANGUAGES = {"en": "English", "fr": "French", "pt": "Portuguese"}
CALLS = {"tr", "trf", "N_"}
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "?": "?", "0": "\0"}


def tokens(text):
    """Identifiers, string literals (decoded) and punctuation, without comments."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
        elif text.startswith("/*", i):
            i = text.find("*/", i + 2)
            i = n if i < 0 else i + 2
        elif c == '"':
            j, out = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                    out.append(ESCAPES.get(text[j], "\\" + text[j]))
                else:
                    out.append(text[j])
                j += 1
            yield ("str", "".join(out))
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            # R"(...)" raw strings are not used for translated texts; skip their prefix.
            yield ("id", text[i:j])
            i = j
        elif c.isspace():
            i += 1
        else:
            yield ("p", c)
            i += 1


def texts_in_code():
    found = {}
    for base in SOURCES:
        for folder, _, files in os.walk(base):
            for name in sorted(files):
                if not name.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(folder, name)
                with open(path, encoding="utf-8") as f:
                    toks = list(tokens(f.read()))
                for k in range(len(toks) - 2):
                    if toks[k][0] == "id" and toks[k][1] in CALLS and toks[k + 1] == ("p", "("):
                        parts, m = [], k + 2
                        while m < len(toks) and toks[m][0] == "str":
                            parts.append(toks[m][1])
                            m += 1
                        if not parts or m >= len(toks) or toks[m] not in (("p", ")"), ("p", ",")):
                            continue
                        text = "".join(parts).split("##")[0]
                        if text:
                            found.setdefault(text, os.path.relpath(path, ROOT))
    return found


def po_unquote(s):
    s = s.strip()
    s = s[s.index('"') + 1 : s.rindex('"')]
    return re.sub(r"\\(.)", lambda m: ESCAPES.get(m.group(1), m.group(1)), s)


def read_po(path):
    entries, cur, field = {}, None, None
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("msgid "):
                if cur and cur["id"]:
                    entries[cur["id"]] = cur["str"]
                cur, field = {"id": po_unquote(line), "str": ""}, "id"
            elif line.startswith("msgstr ") and cur is not None:
                cur["str"], field = po_unquote(line), "str"
            elif line.startswith('"') and cur is not None and field:
                cur[field] += po_unquote(line)
    if cur and cur["id"]:
        entries[cur["id"]] = cur["str"]
    return entries


def po_quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "\\r")
    if "\n" not in s.rstrip("\n"):
        return '"' + s.replace("\n", "\\n") + '"'
    lines = s.split("\n")
    out = ['""']
    for i, l in enumerate(lines):
        last = i == len(lines) - 1
        if last and not l:
            break
        out.append('"' + l + ("" if last else "\\n") + '"')
    return "\n".join(out)


def write_po(path, code, entries):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# plumnin in %s. The msgid is the Spanish text in the code; msgstr its translation.\n" % LANGUAGES[code])
        f.write("# Keep the %-specifiers (%d, %s...) in the same order. Updated with tools/i18n.py --update.\n")
        f.write('msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n"Language: %s\\n"\n' % code)
        for key in sorted(entries):
            f.write("\nmsgid %s\nmsgstr %s\n" % (po_quote(key), po_quote(entries[key])))


SPEC = re.compile(r"%[-+ #0]*(\*|\d+)?(\.(\*|\d+))?(hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcsp%]")


def specifiers(s):
    return [m.group(0) for m in SPEC.finditer(s) if m.group(0) != "%%"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--missing", action="store_true")
    args = ap.parse_args()
    code_texts = texts_in_code()
    problems = 0
    for code in LANGUAGES:
        path = os.path.join(LANG_DIR, code + ".po")
        entries = read_po(path)
        if args.update:
            entries = {k: entries.get(k, "") for k in code_texts}
            write_po(path, code, entries)
        missing = [k for k in code_texts if k not in entries]
        gone = [k for k in entries if k not in code_texts]
        empty = [k for k in code_texts if k in entries and not entries[k]]
        wrong = [k for k in code_texts if entries.get(k) and specifiers(k) != specifiers(entries[k])]
        if args.missing:
            for k in missing + empty:
                print("%s\t%s\t%s" % (code, code_texts[k], k.replace("\n", "\\n")))
        for label, items in (("not in the .po (run tools/i18n.py --update)", missing),
                             ("no longer in the code (run tools/i18n.py --update)", gone),
                             ("untranslated", empty),
                             ("with other %-specifiers than the Spanish", wrong)):
            if items and args.check:
                problems += len(items)
                print("%s.po: %d texts %s:" % (code, len(items), label))
                for k in items[:20]:
                    print("    %s: %r" % (code_texts.get(k, "?"), k))
        if not args.check and not args.missing:
            print("%s.po: %d texts, %d untranslated" % (code, len(code_texts), len(empty) + len(missing)))
    if args.check:
        print("%d texts in tr(), %d problems" % (len(code_texts), problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
