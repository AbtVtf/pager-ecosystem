package status

import (
	"fmt"
	"html/template"
	"time"
)

func templateFuncs() template.FuncMap {
	return template.FuncMap{
		"percent": func(v float64) string {
			return fmt.Sprintf("%.3f%%", v*100)
		},
		"signedPercent": func(v float64) string {
			return fmt.Sprintf("%+.2f%%", v*100)
		},
		"duration": func(seconds int64) string {
			if seconds <= 0 {
				return "—"
			}
			d := time.Duration(seconds) * time.Second
			// Truncate to whole seconds for stable display.
			d = d.Round(time.Second)
			days := d / (24 * time.Hour)
			if days > 0 {
				return fmt.Sprintf("%dd %s", days, (d - days*24*time.Hour).String())
			}
			return d.String()
		},
		"statusClass": func(s string) string {
			switch s {
			case StatusOperational:
				return "ok"
			case StatusDegraded:
				return "warn"
			case StatusDown:
				return "down"
			default:
				return "unknown"
			}
		},
		"statusLabel": func(s string) string {
			switch s {
			case StatusOperational:
				return "Operational"
			case StatusDegraded:
				return "Degraded"
			case StatusDown:
				return "Down"
			default:
				return "Unknown"
			}
		},
		"timeUTC": func(t time.Time) string {
			return t.UTC().Format("2006-01-02 15:04:05 MST")
		},
	}
}

// htmlTemplate is the status.pageros.org page. We inline CSS so the page
// renders correctly even if a CDN is down — the whole point of a status
// page is that it works when nothing else does.
//
// The structure deliberately mirrors well-known status-page conventions
// (overall banner → per-component grid → SLO/metrics row → links) so
// readers don't need to learn a new layout.
const htmlTemplate = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="60">
<title>PagerOS Push Relay — Status</title>
<style>
  :root {
    --bg: #0e1116;
    --bg-card: #161b22;
    --fg: #e6edf3;
    --fg-dim: #8b949e;
    --ok: #3fb950;
    --warn: #d29922;
    --down: #f85149;
    --unknown: #6e7681;
    --rule: #30363d;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Inter", sans-serif;
    background: var(--bg);
    color: var(--fg);
    line-height: 1.5;
  }
  main { max-width: 760px; margin: 0 auto; padding: 32px 16px 64px; }
  h1 { font-size: 20px; margin: 0; }
  header { display: flex; align-items: center; gap: 12px; margin-bottom: 24px; }
  .pill {
    padding: 4px 10px; border-radius: 999px; font-size: 13px; font-weight: 600;
  }
  .pill.ok      { background: rgba(63,185,80,0.15);  color: var(--ok); }
  .pill.warn    { background: rgba(210,153,34,0.15); color: var(--warn); }
  .pill.down    { background: rgba(248,81,73,0.15);  color: var(--down); }
  .pill.unknown { background: rgba(110,118,129,0.2); color: var(--unknown); }
  .banner {
    padding: 18px 20px; border-radius: 8px;
    background: var(--bg-card); border: 1px solid var(--rule);
    margin-bottom: 24px;
  }
  .banner .head { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
  .banner h2 { font-size: 22px; margin: 0; font-weight: 600; }
  .banner small { color: var(--fg-dim); }
  table.components {
    width: 100%; border-collapse: collapse;
    background: var(--bg-card); border: 1px solid var(--rule); border-radius: 8px;
    overflow: hidden;
  }
  table.components tr + tr td { border-top: 1px solid var(--rule); }
  table.components td { padding: 14px 16px; vertical-align: top; }
  table.components td:last-child { text-align: right; white-space: nowrap; }
  .name { font-weight: 600; }
  .detail { color: var(--fg-dim); font-size: 13px; margin-top: 4px; }
  .query { color: var(--fg-dim); font-size: 11px; margin-top: 6px; font-family: ui-monospace, SFMono-Regular, monospace; }
  .slo {
    display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 12px; margin-top: 24px;
  }
  .slo .card {
    background: var(--bg-card); border: 1px solid var(--rule); border-radius: 8px;
    padding: 14px 16px;
  }
  .slo .label { color: var(--fg-dim); font-size: 12px; text-transform: uppercase; letter-spacing: 0.05em; }
  .slo .value { font-size: 20px; font-weight: 600; margin-top: 4px; }
  footer {
    margin-top: 32px; color: var(--fg-dim); font-size: 13px;
    border-top: 1px solid var(--rule); padding-top: 16px;
  }
  footer a { color: var(--fg); }
  footer a:hover { text-decoration: underline; }
</style>
</head>
<body>
<main>
  <header>
    <h1>PagerOS Push Relay</h1>
    <span class="pill {{ statusClass .Overall }}">{{ statusLabel .Overall }}</span>
  </header>

  <div class="banner">
    <div class="head">
      <h2>{{ statusLabel .Overall }}</h2>
      <small>Last updated: {{ timeUTC .Generated }}</small>
    </div>
  </div>

  <table class="components">
    <tbody>
    {{ range .Components }}
      <tr>
        <td>
          <div class="name">{{ .Name }}</div>
          <div class="detail">{{ .Detail }}</div>
          {{ if .MetricQ }}<div class="query">{{ .MetricQ }}</div>{{ end }}
        </td>
        <td>
          <span class="pill {{ statusClass .Status }}">{{ statusLabel .Status }}</span>
        </td>
      </tr>
    {{ end }}
    </tbody>
  </table>

  <div class="slo">
    <div class="card">
      <div class="label">28-day availability</div>
      <div class="value">{{ percent .Availability28d }}</div>
      <div class="detail">Target: {{ percent .AvailabilityTarget }}</div>
    </div>
    <div class="card">
      <div class="label">Error budget remaining</div>
      <div class="value">{{ signedPercent .ErrorBudgetRemaining }}</div>
      <div class="detail">28-day rolling window</div>
    </div>
    <div class="card">
      <div class="label">Queue (entries / devices)</div>
      <div class="value">{{ .QueueEntries }} / {{ .QueueDevices }}</div>
      <div class="detail">Sampled every 30s</div>
    </div>
    <div class="card">
      <div class="label">Current process uptime</div>
      <div class="value">{{ duration .UptimeSeconds }}</div>
      <div class="detail">Resets on each deploy</div>
    </div>
  </div>

  <footer>
    <p>
      Service Level Objective:
      <a href="https://github.com/pageros/pageros/blob/main/push-relay/docs/SLO.md">push-relay/docs/SLO.md</a>
      &nbsp;·&nbsp;
      Specification:
      <a href="https://github.com/pageros/pageros/blob/main/SPEC.md#66-push-relay">SPEC §6.6</a>
      &nbsp;·&nbsp;
      Machine-readable: <a href="/api/status.json">status.json</a>
    </p>
    <p>
      Page auto-refreshes every 60 seconds. Component status is computed from
      the relay's Prometheus instance; the snapshot is cached server-side for
      up to 15 seconds.
    </p>
  </footer>
</main>
</body>
</html>
`
