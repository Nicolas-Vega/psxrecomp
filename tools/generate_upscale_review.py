#!/usr/bin/env python3
"""Generates a local HTML review page comparing each missing_textures/*.png
(the native-decoded "before") against its upscaled counterpart (the "after",
e.g. produced by upscale_missing_textures.py) side by side, with a
checkerboard background so alpha/transparency is actually visible, filters
by source (before) width/height, and a per-image approve/reject toggle
(persisted in the browser's localStorage, keyed by filename, so re-opening
the page keeps your decisions). An "Export approved list" button downloads
a plain-text list of approved filenames (one per line) -- feed that to
apply_upscale_review.py to copy only the approved files into the pack.

This is a plain local HTML file, NOT a published Artifact: it references
the before/after PNGs directly via file:// paths (thousands of images,
no reason to embed them as base64), meant to be opened straight in your
own browser, not shared.

Usage:
    python generate_upscale_review.py --before DIR --after DIR --out review.html
"""
import argparse
import json
from pathlib import Path


def file_uri(p: Path) -> str:
    return p.resolve().as_uri()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True, help="Directory of original (native-decoded) PNGs")
    ap.add_argument("--after", required=True, help="Directory of upscaled PNGs (same filenames as --before)")
    ap.add_argument("--out", default="upscale_review.html", help="Output HTML file path")
    args = ap.parse_args()

    from PIL import Image

    before_dir = Path(args.before)
    after_dir = Path(args.after)

    items = []
    for before_path in sorted(before_dir.glob("*.png")):
        after_path = after_dir / before_path.name
        if not after_path.is_file():
            continue
        with Image.open(before_path) as im:
            bw, bh = im.size
        with Image.open(after_path) as im:
            aw, ah = im.size
        items.append({
            "name": before_path.name,
            "before": file_uri(before_path),
            "after": file_uri(after_path),
            "bw": bw, "bh": bh,
            "aw": aw, "ah": ah,
        })

    print(f"{len(items)} before/after pairs found")

    items_json = json.dumps(items)

    html = HTML_TEMPLATE.replace("__ITEMS_JSON__", items_json).replace("__COUNT__", str(len(items)))
    Path(args.out).write_text(html, encoding="utf-8")
    print(f"wrote {args.out} -- open it in your browser")


HTML_TEMPLATE = r"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>HD texture upscale review (__COUNT__ pairs)</title>
<style>
  :root { color-scheme: dark; }
  body { background: #1a1a1a; color: #eee; font-family: system-ui, sans-serif; margin: 0; padding: 0; }
  #bar {
    position: sticky; top: 0; z-index: 10; background: #232323; border-bottom: 1px solid #3a3a3a;
    padding: 10px 16px; display: flex; gap: 16px; align-items: center; flex-wrap: wrap; font-size: 13px;
  }
  #bar label { display: flex; gap: 6px; align-items: center; white-space: nowrap; }
  #bar input[type=number] { width: 60px; background: #111; color: #eee; border: 1px solid #444; border-radius: 4px; padding: 3px 6px; }
  #bar select { background: #111; color: #eee; border: 1px solid #444; border-radius: 4px; padding: 3px 6px; }
  #bar button { background: #2d6cdf; color: #fff; border: none; border-radius: 4px; padding: 6px 12px; cursor: pointer; font-size: 13px; }
  #bar button:hover { background: #3d7cef; }
  #bar button.secondary { background: #3a3a3a; }
  #bar button.secondary:hover { background: #4a4a4a; }
  #stats { margin-left: auto; color: #999; }
  #grid { padding: 16px; display: grid; grid-template-columns: repeat(auto-fill, minmax(340px, 1fr)); gap: 14px; }
  .card { background: #232323; border: 2px solid #3a3a3a; border-radius: 8px; padding: 10px; }
  .card.approved { border-color: #3fae4a; }
  .card.rejected { border-color: #b23b3b; opacity: 0.55; }
  .card h4 { margin: 0 0 8px; font-size: 12px; font-weight: 500; color: #aaa; word-break: break-all; }
  .imgs { display: flex; gap: 8px; }
  .imgwrap { flex: 1; min-width: 0; }
  .imgwrap .cap { font-size: 11px; color: #888; text-align: center; margin-top: 4px; }
  .checker {
    background-image:
      linear-gradient(45deg, #555 25%, transparent 25%),
      linear-gradient(-45deg, #555 25%, transparent 25%),
      linear-gradient(45deg, transparent 75%, #555 75%),
      linear-gradient(-45deg, transparent 75%, #555 75%);
    background-size: 12px 12px;
    background-position: 0 0, 0 6px, 6px -6px, -6px 0px;
    background-color: #2a2a2a;
    display: flex; align-items: center; justify-content: center;
    min-height: 80px;
  }
  .checker img { max-width: 100%; max-height: 160px; image-rendering: pixelated; display: block; }
  .actions { display: flex; gap: 6px; margin-top: 8px; }
  .actions button { flex: 1; border: none; border-radius: 4px; padding: 6px 0; cursor: pointer; font-size: 12px; }
  .btn-approve { background: #2a4a2c; color: #9fe6a4; }
  .btn-approve.active { background: #3fae4a; color: #fff; }
  .btn-reject { background: #4a2a2a; color: #e69f9f; }
  .btn-reject.active { background: #b23b3b; color: #fff; }
  [hidden] { display: none !important; }
</style>
</head>
<body>
<div id="bar">
  <label>Ancho orig. min <input type="number" id="minW" placeholder="0"></label>
  <label>Ancho orig. max <input type="number" id="maxW" placeholder="∞"></label>
  <label>Alto orig. min <input type="number" id="minH" placeholder="0"></label>
  <label>Alto orig. max <input type="number" id="maxH" placeholder="∞"></label>
  <label>Estado
    <select id="stateFilter">
      <option value="all">Todas</option>
      <option value="undecided">Sin decidir</option>
      <option value="approved">Aprobadas</option>
      <option value="rejected">Rechazadas</option>
    </select>
  </label>
  <button class="secondary" id="applyFilter">Filtrar</button>
  <button class="secondary" id="approveAllVisible">Aprobar visibles</button>
  <button id="exportBtn">Exportar aprobadas (.txt)</button>
  <span id="stats"></span>
</div>
<div id="grid"></div>
<script>
const ITEMS = __ITEMS_JSON__;
const STORE_KEY = "vsr_upscale_review_decisions_v1";

function loadDecisions() {
  try { return JSON.parse(localStorage.getItem(STORE_KEY) || "{}"); }
  catch (e) { return {}; }
}
function saveDecisions(d) {
  try { localStorage.setItem(STORE_KEY, JSON.stringify(d)); } catch (e) {}
}
let decisions = loadDecisions();

const grid = document.getElementById("grid");
const statsEl = document.getElementById("stats");

function passesFilter(item) {
  const minW = parseInt(document.getElementById("minW").value) || 0;
  const maxW = parseInt(document.getElementById("maxW").value) || Infinity;
  const minH = parseInt(document.getElementById("minH").value) || 0;
  const maxH = parseInt(document.getElementById("maxH").value) || Infinity;
  if (item.bw < minW || item.bw > maxW) return false;
  if (item.bh < minH || item.bh > maxH) return false;
  const st = document.getElementById("stateFilter").value;
  const d = decisions[item.name];
  if (st === "undecided" && d) return false;
  if (st === "approved" && d !== "approved") return false;
  if (st === "rejected" && d !== "rejected") return false;
  return true;
}

function cardHtml(item) {
  const d = decisions[item.name] || "";
  return `
    <div class="card ${d}" data-name="${item.name}">
      <h4>${item.name} &mdash; ${item.bw}x${item.bh} → ${item.aw}x${item.ah}</h4>
      <div class="imgs">
        <div class="imgwrap">
          <div class="checker"><img loading="lazy" src="${item.before}"></div>
          <div class="cap">antes</div>
        </div>
        <div class="imgwrap">
          <div class="checker"><img loading="lazy" src="${item.after}"></div>
          <div class="cap">después</div>
        </div>
      </div>
      <div class="actions">
        <button class="btn-approve ${d === 'approved' ? 'active' : ''}" data-act="approved">✓ Aprobar</button>
        <button class="btn-reject ${d === 'rejected' ? 'active' : ''}" data-act="rejected">✗ Rechazar</button>
      </div>
    </div>`;
}

let visible = ITEMS;

function render() {
  visible = ITEMS.filter(passesFilter);
  grid.innerHTML = visible.map(cardHtml).join("");
  updateStats();
}

function updateStats() {
  const approved = Object.values(decisions).filter(v => v === "approved").length;
  const rejected = Object.values(decisions).filter(v => v === "rejected").length;
  statsEl.textContent = `${visible.length} visibles / ${ITEMS.length} total — ${approved} aprobadas, ${rejected} rechazadas`;
}

grid.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-act]");
  if (!btn) return;
  const card = btn.closest(".card");
  const name = card.dataset.name;
  const act = btn.dataset.act;
  decisions[name] = decisions[name] === act ? undefined : act;
  if (decisions[name] === undefined) delete decisions[name];
  saveDecisions(decisions);
  card.className = "card " + (decisions[name] || "");
  card.querySelectorAll("button").forEach(b => b.classList.remove("active"));
  if (decisions[name]) card.querySelector(`[data-act="${decisions[name]}"]`).classList.add("active");
  updateStats();
});

document.getElementById("applyFilter").addEventListener("click", render);
document.getElementById("approveAllVisible").addEventListener("click", () => {
  visible.forEach(item => { decisions[item.name] = "approved"; });
  saveDecisions(decisions);
  render();
});
document.getElementById("exportBtn").addEventListener("click", () => {
  const approved = Object.entries(decisions).filter(([,v]) => v === "approved").map(([k]) => k);
  const blob = new Blob([approved.join("\n") + "\n"], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = "approved_textures.txt";
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
});

render();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
