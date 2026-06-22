#!/usr/bin/env python3
"""
Genera ARQUITECTURA.html (y, si hay Microsoft Edge, ARQUITECTURA.pdf) a partir
de ARQUITECTURA.md.

Uso:
    python docs/build_docs.py

Dependencias: markdown, pygments  (pip install markdown pygments)
El PDF se genera con Microsoft Edge en modo headless (no requiere instalar nada).
"""
import os
import re
import shutil
import subprocess
import sys

import markdown

DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(DOCS_DIR)
MD_PATH = os.path.join(DOCS_DIR, "ARQUITECTURA.md")
HTML_PATH = os.path.join(DOCS_DIR, "ARQUITECTURA.html")
PDF_PATH = os.path.join(DOCS_DIR, "ARQUITECTURA.pdf")


def rel_repo(path: str) -> str:
    """Convierte una ruta (absoluta o no) a relativa del repo, con '/'."""
    p = path.replace("\\", "/")
    root = REPO_ROOT.replace("\\", "/")
    if p.lower().startswith(root.lower()):
        p = p[len(root):].lstrip("/")
    return p


def preprocess_refs(text: str) -> str:
    """
    Convierte las etiquetas de cita de Devin en codigo en linea legible:
      <ref_file file="...\\servidor.c" />            -> `servidor.c`
      <ref_snippet file="...\\nave.c" lines="1-9" />  -> `nave.c:1-9`
    """
    def file_sub(m):
        return "`" + rel_repo(m.group(1)) + "`"

    def snippet_sub(m):
        return "`" + rel_repo(m.group(1)) + ":" + m.group(2) + "`"

    text = re.sub(r'<ref_file\s+file="([^"]+)"\s*/>', file_sub, text)
    text = re.sub(
        r'<ref_snippet\s+file="([^"]+)"\s+lines="([^"]+)"\s*/>',
        snippet_sub, text,
    )
    return text


CSS = """
:root { --fg:#1f2328; --muted:#656d76; --border:#d0d7de; --accent:#0969da;
        --bg-code:#f6f8fa; --bg-th:#f0f3f6; }
* { box-sizing: border-box; }
body { color: var(--fg); font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
       font-size: 11pt; line-height: 1.55; max-width: 980px; margin: 0 auto; padding: 28px 36px; }
h1,h2,h3,h4 { line-height: 1.25; margin-top: 1.4em; margin-bottom: .5em; font-weight: 600; }
h1 { font-size: 2em; border-bottom: 2px solid var(--border); padding-bottom: .3em; }
h2 { font-size: 1.5em; border-bottom: 1px solid var(--border); padding-bottom: .25em; margin-top: 1.8em; }
h3 { font-size: 1.2em; }
h4 { font-size: 1.05em; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
p, li { margin: .4em 0; }
blockquote { margin: .8em 0; padding: .2em 1em; color: var(--muted);
             border-left: 4px solid var(--accent); background: #f6f8fa; }
blockquote strong { color: var(--fg); }
code { font-family: "Cascadia Code", "Consolas", ui-monospace, SFMono-Regular, Menlo, monospace;
       font-size: 85%; background: var(--bg-code); padding: .15em .4em; border-radius: 6px; }
pre { background: var(--bg-code); padding: 14px 16px; border-radius: 8px; overflow: auto;
      border: 1px solid var(--border); font-size: 9.5pt; line-height: 1.4; }
pre code { background: none; padding: 0; font-size: inherit; white-space: pre; }
table { border-collapse: collapse; width: 100%; margin: 1em 0; font-size: 10pt; }
th, td { border: 1px solid var(--border); padding: 7px 11px; text-align: left; vertical-align: top; }
th { background: var(--bg-th); font-weight: 600; }
tr:nth-child(even) td { background: #fafbfc; }
hr { border: none; border-top: 1px solid var(--border); margin: 2em 0; }
.toc-wrap { background: #f6f8fa; border: 1px solid var(--border); border-radius: 8px;
            padding: 4px 18px; margin: 1em 0; }
@media print {
  body { max-width: none; padding: 0; font-size: 10.5pt; }
  h1, h2, h3 { page-break-after: avoid; }
  pre, table, blockquote { page-break-inside: avoid; }
  a { color: var(--fg); }
}
"""


def build_html() -> str:
    with open(MD_PATH, encoding="utf-8") as f:
        raw = f.read()
    raw = preprocess_refs(raw)

    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "codehilite", "toc",
                    "sane_lists", "attr_list"],
        extension_configs={"codehilite": {"guess_lang": False, "noclasses": True}},
    )
    body = md.convert(raw)

    html = f"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CosmiKernel - Arquitectura y Diseno de SO</title>
<style>{CSS}</style>
</head>
<body>
{body}
</body>
</html>
"""
    with open(HTML_PATH, "w", encoding="utf-8") as f:
        f.write(html)
    return HTML_PATH


def find_edge() -> str | None:
    candidates = [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return shutil.which("msedge")


def build_pdf() -> bool:
    edge = find_edge()
    if not edge:
        print("PDF: no se encontro Microsoft Edge; solo se genero el HTML.")
        return False
    file_url = "file:///" + HTML_PATH.replace("\\", "/")
    cmd = [
        edge, "--headless", "--disable-gpu", "--no-first-run",
        "--no-pdf-header-footer",
        f"--print-to-pdf={PDF_PATH}", file_url,
    ]
    try:
        subprocess.run(cmd, timeout=120, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as e:  # noqa: BLE001
        print(f"PDF: fallo Edge ({e}).")
        return False
    return os.path.exists(PDF_PATH) and os.path.getsize(PDF_PATH) > 0


if __name__ == "__main__":
    build_html()
    print(f"HTML generado: {HTML_PATH}")
    if build_pdf():
        size_kb = os.path.getsize(PDF_PATH) / 1024
        print(f"PDF  generado: {PDF_PATH} ({size_kb:.0f} KB)")
    else:
        sys.exit(0)
