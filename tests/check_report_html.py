#!/usr/bin/env python3
"""check_report_html.py — miniprof_report.py --html on snapshot files:
the page is produced, tags balance, no raw None/nan leaks, and every
metrics block present in the snapshot has its section (or says why not)."""
import html.parser, json, os, subprocess, sys, tempfile

VOID = {"meta", "br", "hr", "img", "input", "link", "path", "rect", "line", "circle"}
here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, "..", "tools", "miniprof_report.py")


class Check(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.stack, self.ids, self.err, self.text = [], set(), [], []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        if tag not in VOID:
            self.stack.append(tag)

    def handle_startendtag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.err.append(f"unbalanced </{tag}> (open: {self.stack[-3:]})")
            return
        self.stack.pop()

    def handle_data(self, d):
        self.text.append(d)


fails = 0
for f in sys.argv[1:]:
    doc = json.load(open(f))
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "r.html")
        r = subprocess.run([sys.executable, tool, f, "--html", out], capture_output=True, text=True)
        if r.returncode:
            print(f"FAIL  {f}: report exited {r.returncode}\n{r.stderr}")
            fails += 1
            continue
        page = open(out).read()
    c = Check()
    c.feed(page)
    text = " ".join(c.text)
    errs = list(c.err)
    if c.stack:
        errs.append(f"unclosed tags {c.stack}")
    for bad in (" None", "nan%", " nan ", "inf GB"):
        if bad in text:
            errs.append(f"text contains {bad!r}")
    m = doc.get("metrics") or {}
    want = {"parallel": "parallel", "openmp": "openmp", "counters": "counters", "topdown": "topdown",
            "memory": "memory", "roofline": "roofline"}
    for k, sid in want.items():
        if m.get(k) is not None and sid not in c.ids:
            errs.append(f"no section #{sid} for metrics.{k}")
    if "<script" in page and page.count("<script") != 1:
        errs.append("more than the one inline script")
    if "http://" in page or "https://" in page:
        errs.append("external URL in a self-contained page")
    if errs:
        fails += 1
        print(f"FAIL  {f}")
        for e in errs:
            print(f"      {e}")
    else:
        print(f"ok    {f} ({len(page) // 1024} KiB)")
print("ALL HTML REPORT CHECKS PASSED" if not fails else f"{fails} HTML REPORT CHECK(S) FAILED")
sys.exit(1 if fails else 0)
