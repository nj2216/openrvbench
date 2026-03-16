#!/usr/bin/env python3
# ─── OpenRVBench :: HTML Report Generator ────────────────────────────────────
# Generates a standalone, self-contained HTML report with embedded CSS + JS.
# No external dependencies needed on the target device.
# ─────────────────────────────────────────────────────────────────────────────
"""Generate HTML benchmark report from result data."""

import json
import pathlib
import datetime
from typing import Optional


# ─── Score normalisation for radar chart ─────────────────────────────────────
# (max expected score for each category — board-class reference)
SCORE_BASELINES = {
    "cpu":     2000.0,
    "vector":  500.0,
    "memory":  2000.0,
    "crypto":  500.0,
    "storage": 1000.0,
    "network": 5000.0,
    "ai":      1000.0,
    "thermal": 900.0,
}


def _fmt(v, precision=2):
    if isinstance(v, float):
        return f"{v:.{precision}f}"
    if isinstance(v, bool):
        return "yes" if v else "no"
    return str(v)


def _metric_rows(metrics: list) -> str:
    rows = []
    for m in metrics:
        name = m.get("name", "").replace("_", " ").title()
        val  = _fmt(m.get("value", ""))
        unit = m.get("unit", "")
        desc = m.get("desc", "")
        rows.append(
            f'<tr><td class="mn">{name}</td>'
            f'<td class="mv">{val}</td>'
            f'<td class="mu">{unit}</td>'
            f'<td class="md">{desc}</td></tr>'
        )
    return "\n".join(rows)


def _bench_cards(bench_results: list) -> str:
    cards = []
    for r in bench_results:
        bid    = r.get("bench_id", "?")
        bname  = r.get("bench_name", bid)
        ok     = r.get("passed", False)
        score  = r.get("score", 0.0)
        unit   = r.get("score_unit", "pts")
        dur    = r.get("duration_sec", 0.0)
        err    = r.get("error", "")
        status_cls = "pass" if ok else "fail"
        status_txt = "PASS" if ok else "FAIL"

        # Icon map
        icons = {
            "cpu": "⚙️", "vector": "🧮", "memory": "💾",
            "crypto": "🔐", "storage": "📀", "network": "🌐",
            "ai": "🤖", "thermal": "🌡️"
        }
        icon = icons.get(bid, "📊")

        err_html = ""
        if not ok and err:
            err_html = f'<div class="bench-error">{err}</div>'

        metric_html = ""
        if ok:
            metrics = r.get("metrics", [])
            visible = [m for m in metrics
                       if not isinstance(m.get("value"), bool)][:8]
            if visible:
                metric_html = (
                    '<table class="metric-table">'
                    '<tr><th>Metric</th><th>Value</th>'
                    '<th>Unit</th><th>Description</th></tr>'
                    + _metric_rows(visible)
                    + '</table>'
                )

        cards.append(f'''
<div class="bench-card {status_cls}">
  <div class="bench-header">
    <span class="bench-icon">{icon}</span>
    <span class="bench-name">{bname}</span>
    <span class="bench-status {status_cls}">{status_txt}</span>
  </div>
  <div class="bench-score">
    <span class="score-num">{_fmt(score, 1)}</span>
    <span class="score-unit">{unit}</span>
    <span class="bench-dur">({dur:.1f}s)</span>
  </div>
  {err_html}
  {metric_html}
</div>''')
    return "\n".join(cards)


def _radar_data(bench_results: list) -> str:
    """Build Chart.js radar data from bench scores."""
    labels = []
    values = []
    for r in bench_results:
        bid   = r.get("bench_id", "")
        score = r.get("score", 0.0) if r.get("passed") else 0.0
        base  = SCORE_BASELINES.get(bid, 1000.0)
        pct   = min(100.0, score / base * 100.0)
        labels.append(r.get("bench_id", "?").upper())
        values.append(round(pct, 1))
    return json.dumps(labels), json.dumps(values)


def _score_bar_data(bench_results: list) -> str:
    labels = []
    scores = []
    colors = []
    palette = ["#6ee7b7","#93c5fd","#fcd34d","#f9a8d4",
               "#a5b4fc","#6d28d9","#f87171","#34d399"]
    for i, r in enumerate(bench_results):
        if r.get("passed"):
            labels.append(r.get("bench_id", "?").upper())
            scores.append(round(r.get("score", 0.0), 1))
            colors.append(palette[i % len(palette)])
    return json.dumps(labels), json.dumps(scores), json.dumps(colors)


def generate_html_report(board_info: dict,
                          bench_results: list,
                          output_path: pathlib.Path):
    """Write a standalone HTML report to output_path."""

    board_name    = board_info.get("board", "Unknown Board")
    isa           = board_info.get("isa",   "unknown")
    cores         = board_info.get("cores", "?")
    ram           = board_info.get("ram_gb", "?")
    kernel        = board_info.get("kernel", "?")
    os_str        = board_info.get("os",    "?")
    has_rvv       = board_info.get("has_rvv", False)
    rvv_badge     = "✓ RVV" if has_rvv else "✗ No RVV"
    rvv_cls       = "badge-green" if has_rvv else "badge-yellow"
    date_str      = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")

    total_score   = sum(r.get("score", 0) for r in bench_results if r.get("passed"))
    n_pass        = sum(1 for r in bench_results if r.get("passed"))
    n_total       = len(bench_results)

    bench_cards   = _bench_cards(bench_results)
    radar_labels, radar_values = _radar_data(bench_results)
    bar_labels, bar_scores, bar_colors = _score_bar_data(bench_results)

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>OpenRVBench — {board_name}</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
  :root {{
    --bg:       #0f172a;
    --surface:  #1e293b;
    --border:   #334155;
    --text:     #e2e8f0;
    --muted:    #94a3b8;
    --green:    #4ade80;
    --yellow:   #facc15;
    --red:      #f87171;
    --blue:     #60a5fa;
    --purple:   #a78bfa;
    --cyan:     #67e8f9;
  }}
  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    font-family: 'JetBrains Mono', 'Fira Code', 'Consolas', monospace;
    background: var(--bg); color: var(--text);
    min-height: 100vh; padding: 2rem;
  }}
  a {{ color: var(--blue); text-decoration: none; }}

  /* Header */
  .header {{
    display: flex; align-items: center; gap: 1.5rem;
    border-bottom: 1px solid var(--border);
    padding-bottom: 1.5rem; margin-bottom: 2rem;
  }}
  .logo {{ font-size: 2.5rem; }}
  .title-block h1 {{ font-size: 1.8rem; color: var(--cyan); font-weight: 700; }}
  .title-block p  {{ color: var(--muted); font-size: 0.85rem; margin-top: 0.25rem; }}

  /* System info */
  .sysinfo {{
    display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
    gap: 1rem; margin-bottom: 2rem;
  }}
  .sysinfo-card {{
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 8px; padding: 1rem;
  }}
  .sysinfo-card .label {{ font-size: 0.7rem; color: var(--muted); text-transform: uppercase; }}
  .sysinfo-card .value {{ font-size: 1.0rem; color: var(--text); margin-top: 0.3rem; font-weight: 600; }}

  /* Badges */
  .badge-green  {{ color: var(--green);  }}
  .badge-yellow {{ color: var(--yellow); }}
  .badge-red    {{ color: var(--red);    }}

  /* Score hero */
  .score-hero {{
    text-align: center; padding: 2rem;
    background: linear-gradient(135deg, #1e293b 0%, #0f172a 100%);
    border: 1px solid var(--border); border-radius: 12px;
    margin-bottom: 2rem;
  }}
  .score-hero .big-score {{ font-size: 4rem; font-weight: 800; color: var(--cyan); }}
  .score-hero .score-label {{ color: var(--muted); margin-top: 0.5rem; }}
  .score-hero .pass-info {{ color: var(--muted); margin-top: 0.5rem; font-size: 0.85rem; }}

  /* Charts */
  .charts-grid {{
    display: grid; grid-template-columns: 1fr 1fr;
    gap: 1.5rem; margin-bottom: 2rem;
  }}
  @media(max-width: 768px) {{ .charts-grid {{ grid-template-columns: 1fr; }} }}
  .chart-card {{
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 12px; padding: 1.5rem;
  }}
  .chart-card h3 {{
    font-size: 0.85rem; color: var(--muted);
    text-transform: uppercase; letter-spacing: 0.1em;
    margin-bottom: 1rem;
  }}

  /* Benchmark cards */
  .section-title {{
    font-size: 0.8rem; color: var(--muted);
    text-transform: uppercase; letter-spacing: 0.1em;
    margin-bottom: 1rem; margin-top: 2rem;
    border-bottom: 1px solid var(--border); padding-bottom: 0.5rem;
  }}
  .bench-grid {{
    display: grid; grid-template-columns: repeat(auto-fill, minmax(480px, 1fr));
    gap: 1.5rem;
  }}
  @media(max-width: 600px) {{ .bench-grid {{ grid-template-columns: 1fr; }} }}
  .bench-card {{
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 10px; padding: 1.25rem;
    border-left: 3px solid var(--border);
  }}
  .bench-card.pass {{ border-left-color: var(--green); }}
  .bench-card.fail {{ border-left-color: var(--red);   }}

  .bench-header {{
    display: flex; align-items: center; gap: 0.75rem; margin-bottom: 0.75rem;
  }}
  .bench-icon   {{ font-size: 1.25rem; }}
  .bench-name   {{ font-weight: 700; font-size: 0.95rem; flex: 1; }}
  .bench-status {{ font-size: 0.7rem; font-weight: 700; padding: 0.2rem 0.5rem;
                   border-radius: 4px; }}
  .bench-status.pass {{ background: rgba(74,222,128,0.15); color: var(--green); }}
  .bench-status.fail {{ background: rgba(248,113,113,0.15); color: var(--red); }}

  .bench-score  {{
    display: flex; align-items: baseline; gap: 0.5rem;
    margin-bottom: 0.75rem;
  }}
  .score-num    {{ font-size: 2rem; font-weight: 800; color: var(--cyan); }}
  .score-unit   {{ font-size: 0.85rem; color: var(--muted); }}
  .bench-dur    {{ font-size: 0.75rem; color: var(--muted); margin-left: auto; }}
  .bench-error  {{
    font-size: 0.8rem; color: var(--red);
    background: rgba(248,113,113,0.05);
    border: 1px solid rgba(248,113,113,0.2);
    padding: 0.5rem; border-radius: 4px; margin-top: 0.5rem;
  }}

  /* Metrics table */
  .metric-table {{
    width: 100%; border-collapse: collapse; font-size: 0.78rem;
    margin-top: 0.5rem;
  }}
  .metric-table th {{
    text-align: left; padding: 0.3rem 0.5rem;
    color: var(--muted); font-weight: 600;
    border-bottom: 1px solid var(--border);
    font-size: 0.7rem; text-transform: uppercase;
  }}
  .metric-table td {{ padding: 0.3rem 0.5rem; border-bottom: 1px solid rgba(51,65,85,0.5); }}
  .metric-table .mv {{ color: var(--cyan); font-weight: 600; }}
  .metric-table .mu {{ color: var(--muted); }}
  .metric-table .md {{ color: var(--muted); font-style: italic; }}

  /* Footer */
  .footer {{
    text-align: center; color: var(--muted); font-size: 0.75rem;
    margin-top: 3rem; padding-top: 1.5rem;
    border-top: 1px solid var(--border);
  }}
</style>
</head>
<body>

<div class="header">
  <div class="logo">🖥️</div>
  <div class="title-block">
    <h1>OpenRVBench</h1>
    <p>RISC-V Benchmark Suite v1.1.0&nbsp;·&nbsp; {date_str}</p>
  </div>
</div>

<!-- System Info -->
<div class="sysinfo">
  <div class="sysinfo-card">
    <div class="label">Board</div>
    <div class="value">{board_name}</div>
  </div>
  <div class="sysinfo-card">
    <div class="label">ISA</div>
    <div class="value">{isa}</div>
  </div>
  <div class="sysinfo-card">
    <div class="label">Vector Ext</div>
    <div class="value {rvv_cls}">{rvv_badge}</div>
  </div>
  <div class="sysinfo-card">
    <div class="label">CPU Cores</div>
    <div class="value">{cores}</div>
  </div>
  <div class="sysinfo-card">
    <div class="label">RAM</div>
    <div class="value">{ram} GB</div>
  </div>
  <div class="sysinfo-card">
    <div class="label">Kernel</div>
    <div class="value">{kernel}</div>
  </div>
  <div class="sysinfo-card">
    <div class="label">OS</div>
    <div class="value">{os_str}</div>
  </div>
</div>

<!-- Score Hero -->
<div class="score-hero">
  <div class="big-score">{total_score:.0f}</div>
  <div class="score-label">Overall Score (pts)</div>
  <div class="pass-info">{n_pass} of {n_total} benchmarks passed</div>
</div>

<!-- Charts -->
<div class="charts-grid">
  <div class="chart-card">
    <h3>Performance Radar (%age of baseline)</h3>
    <canvas id="radarChart" height="260"></canvas>
  </div>
  <div class="chart-card">
    <h3>Score by Category</h3>
    <canvas id="barChart" height="260"></canvas>
  </div>
</div>

<!-- Benchmark Detail Cards -->
<div class="section-title">Detailed Results</div>
<div class="bench-grid">
{bench_cards}
</div>

<div class="footer">
  Generated by <strong>OpenRVBench v1.0</strong> &nbsp;·&nbsp;
  <a href="https://github.com/your-org/openrvbench">github.com/your-org/openrvbench</a>
  &nbsp;·&nbsp; {date_str}
</div>

<script>
const radarLabels = {radar_labels};
const radarValues = {radar_values};
const barLabels   = {bar_labels};
const barScores   = {bar_scores};
const barColors   = {bar_colors};

const chartDefaults = {{
  color: '#94a3b8',
  borderColor: '#334155',
}};
Chart.defaults.color = '#94a3b8';

// Radar
new Chart(document.getElementById('radarChart'), {{
  type: 'radar',
  data: {{
    labels: radarLabels,
    datasets: [{{
      label: 'Score %',
      data: radarValues,
      backgroundColor: 'rgba(103,232,249,0.15)',
      borderColor: '#67e8f9',
      pointBackgroundColor: '#67e8f9',
      pointRadius: 4,
      borderWidth: 2,
    }}]
  }},
  options: {{
    responsive: true,
    scales: {{
      r: {{
        min: 0, max: 100,
        grid:       {{ color: '#334155' }},
        angleLines: {{ color: '#334155' }},
        ticks:      {{ color: '#94a3b8', backdropColor: 'transparent',
                       stepSize: 25 }},
        pointLabels: {{ color: '#e2e8f0', font: {{ size: 11 }} }},
      }}
    }},
    plugins: {{ legend: {{ display: false }} }}
  }}
}});

// Bar
new Chart(document.getElementById('barChart'), {{
  type: 'bar',
  data: {{
    labels: barLabels,
    datasets: [{{
      label: 'Score',
      data: barScores,
      backgroundColor: barColors,
      borderRadius: 4,
    }}]
  }},
  options: {{
    responsive: true,
    plugins: {{ legend: {{ display: false }} }},
    scales: {{
      x: {{ grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }},
      y: {{ grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }},
    }}
  }}
}});
</script>
</body>
</html>"""

    output_path = pathlib.Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(html, encoding="utf-8")
    return output_path


# ─── CLI usage ────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    import sys
    if len(sys.argv) < 3:
        print("Usage: report_generator.py result.json output.html")
        sys.exit(1)
    data = json.loads(pathlib.Path(sys.argv[1]).read_text())
    out  = generate_html_report(
        data.get("board", {}),
        data.get("benchmarks", []),
        pathlib.Path(sys.argv[2])
    )
    print(f"Report written → {out}")
