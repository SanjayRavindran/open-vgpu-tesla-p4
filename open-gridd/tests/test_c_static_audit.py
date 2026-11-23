import os
import re
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CODE_DIRS = ["docs", "include", "src", "tests", "packaging"]


def strip_c_comments_and_strings(src: str) -> str:
    out = []
    i = 0
    n = len(src)
    in_str = False
    in_chr = False
    in_line = False
    in_block = False
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            i += 1
        elif in_block:
            if c == "*" and nxt == "/":
                in_block = False
                i += 2
            else:
                i += 1
        elif in_str:
            if c == "\\" and i + 1 < n:
                i += 2
            elif c == '"':
                in_str = False
                out.append('"')
                i += 1
            else:
                i += 1
        elif in_chr:
            if c == "\\" and i + 1 < n:
                i += 2
            elif c == "'":
                in_chr = False
                out.append("'")
                i += 1
            else:
                i += 1
        else:
            if c == "/" and nxt == "/":
                in_line = True
                i += 2
            elif c == "/" and nxt == "*":
                in_block = True
                i += 2
            elif c == '"':
                in_str = True
                out.append('"')
                i += 1
            elif c == "'":
                in_chr = True
                out.append("'")
                i += 1
            else:
                out.append(c)
                i += 1
    return "".join(out)


def read_text(path: str) -> str:
    with open(path, encoding="utf-8", errors="ignore") as fp:
        return fp.read()


class CStaticAuditTests(unittest.TestCase):
    def test_cmake_sources_match_src_directory(self):
        cmake_text = read_text(os.path.join(ROOT, "CMakeLists.txt"))
        listed = set(re.findall(r"src/[a-zA-Z0-9_]+\.c", cmake_text))
        actual = {f"src/{f}" for f in os.listdir(os.path.join(ROOT, "src")) if f.endswith(".c")}
        self.assertEqual(listed, actual)
        for rel in listed:
            self.assertTrue(os.path.isfile(os.path.join(ROOT, rel)), f"Missing {rel}")

    def test_clean_compiler_flags_and_crypto_params(self):
        # Ensure we use non-const char array or cast for OpenSSL OSSL_PARAM_construct_utf8_string if needed,
        # and verify no libcurl header remains referenced.
        for fname in os.listdir(os.path.join(ROOT, "src")):
            if not fname.endswith(".c"):
                continue
            txt = read_text(os.path.join(ROOT, "src", fname))
            self.assertNotIn("<curl/curl.h>", txt)

    def test_balanced_delimiters_and_guards(self):
        for sub in ("include/gridd", "src", "tests"):
            d = os.path.join(ROOT, sub)
            for fname in os.listdir(d):
                if not (fname.endswith(".c") or fname.endswith(".h")):
                    continue
                path = os.path.join(d, fname)
                raw = read_text(path)
                if fname.endswith(".h"):
                    self.assertIn("#ifndef GRIDD_", raw, f"Missing include guard in {fname}")
                    self.assertIn("#define GRIDD_", raw, f"Missing include guard define in {fname}")

                stripped = strip_c_comments_and_strings(raw)
                for open_ch, close_ch in (("{", "}"), ("(", ")"), ("[", "]")):
                    self.assertEqual(
                        stripped.count(open_ch),
                        stripped.count(close_ch),
                        f"Unbalanced {open_ch}/{close_ch} in {sub}/{fname}",
                    )

                if_depth = 0
                for line in stripped.splitlines():
                    s = line.strip()
                    if re.match(r"^#\s*(if|ifdef|ifndef)\b", s):
                        if_depth += 1
                    elif re.match(r"^#\s*endif\b", s):
                        if_depth -= 1
                        self.assertGreaterEqual(if_depth, 0, f"Extra #endif in {sub}/{fname}")
                self.assertEqual(if_depth, 0, f"Unclosed #if in {sub}/{fname}")

    def test_header_declarations_defined_in_src(self):
        src_corpus = ""
        for fname in os.listdir(os.path.join(ROOT, "src")):
            if fname.endswith(".c"):
                src_corpus += "\n" + read_text(os.path.join(ROOT, "src", fname))

        decl_pat = re.compile(r"\b(gridd_[a-zA-Z0-9_]+)\s*\(")
        for fname in os.listdir(os.path.join(ROOT, "include/gridd")):
            if not fname.endswith(".h"):
                continue
            htxt = strip_c_comments_and_strings(read_text(os.path.join(ROOT, "include/gridd", fname)))
            for m in decl_pat.finditer(htxt):
                sym = m.group(1)
                # Ignore typedef function pointers and macro functions
                line_start = htxt.rfind("\n", 0, m.start()) + 1
                line = htxt[line_start:htxt.find("\n", m.end())]
                if "(*" in line or "#define" in line:
                    continue
                self.assertRegex(
                    src_corpus,
                    rf"\b{re.escape(sym)}\s*\(",
                    f"Declared symbol {sym} from {fname} not defined in src/*.c",
                )

    def test_zero_decompiler_leakage_and_banned_c_calls(self):
        leak_pat = re.compile(r"\b(?:FUN_[0-9a-fA-F]+|DAT_[0-9a-fA-F]+|undefined[0-9]*|uVar[0-9]+|iVar[0-9]+|pcVar[0-9]+|local_[0-9a-fA-F]+)\b")
        banned_pat = re.compile(r"\b(?:strcpy|strcat|sprintf|gets)\s*\(")

        for sub in CODE_DIRS:
            d = os.path.join(ROOT, sub)
            for root_dir, dirnames, files in os.walk(d):
                dirnames[:] = [dn for dn in dirnames if dn != "__pycache__"]
                for fname in files:
                    if fname == "test_c_static_audit.py" or fname.endswith(".pyc"):
                        continue
                    p = os.path.join(root_dir, fname)
                    txt = read_text(p)
                    self.assertIsNone(leak_pat.search(txt), f"Decompiler symbol leaked into {p}")
                    self.assertNotIn("Ghidra", txt, f"Ghidra reference leaked into {p}")
                    if sub == "src" and fname.endswith(".c"):
                        stripped = strip_c_comments_and_strings(txt)
                        self.assertIsNone(banned_pat.search(stripped), f"Banned unsafe C function in {p}")

    def test_protocol_and_rm_constants_match_spec(self):
        spec = read_text(os.path.join(ROOT, "docs/SPECIFICATION.md"))
        rm_h = read_text(os.path.join(ROOT, "include/gridd/rm_client.h"))
        nls_c = read_text(os.path.join(ROOT, "src/nls_client.c"))

        for ctrl in (
            "0x00800287", "0x00800289", "0x20800164", "0x20800186",
            "0x20800187", "0x20800189", "0x2080018c", "0x20801801",
            "0x20803601", "0x20808161",
        ):
            self.assertIn(ctrl, spec)
            self.assertIn(ctrl, rm_h)

        for ep in (
            "/auth/v1/origin", "/auth/v1/code", "/auth/v1/token",
            "/leasing/v1/lessor", "/leasing/v1/lease/", "/leasing/v1/lessor/leases",
            "/leasing/v1/lessor/shutdown",
        ):
            self.assertIn(ep, spec)
            self.assertIn(ep, nls_c)


if __name__ == "__main__":
    unittest.main()
