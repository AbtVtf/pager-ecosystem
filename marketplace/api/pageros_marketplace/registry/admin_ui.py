"""Server-rendered admin moderation dashboard (MKT-006).

Renders a single HTML page summarizing:
  * Open reports awaiting moderator action.
  * Registered apps with their current trust tags.
  * The 50 most recent moderation log entries.

The UI is intentionally thin — no JS framework, no build step. It posts
straight to the JSON admin endpoints via tiny inline fetch handlers so
authenticated curl + browser flows look the same.
"""

from __future__ import annotations

import html
import json
from datetime import datetime
from typing import Iterable

from .moderation import LogEntry, Report
from .store import AppEntry


def render_admin_index(
    *,
    apps: Iterable[AppEntry],
    open_reports: Iterable[Report],
    recent_log: Iterable[LogEntry],
    allowed_tags: list[str],
) -> str:
    apps_list = list(apps)
    reports_list = list(open_reports)
    log_list = list(recent_log)

    return _PAGE.format(
        reports_section=_render_reports(reports_list),
        apps_section=_render_apps(apps_list, allowed_tags),
        log_section=_render_log(log_list),
        allowed_tags_json=html.escape(json.dumps(allowed_tags)),
        report_count=len(reports_list),
        app_count=len(apps_list),
    )


def _render_reports(reports: list[Report]) -> str:
    if not reports:
        return '<p class="empty">No open reports.</p>'
    rows = []
    for r in reports:
        rows.append(
            "<tr>"
            f"<td><code>{html.escape(r.id)}</code></td>"
            f"<td><code>{html.escape(r.app_id)}</code></td>"
            f"<td>{html.escape(r.reason)}</td>"
            f"<td>{html.escape(r.reporter_contact or '—')}</td>"
            f"<td>{_fmt_ts(r.filed_at)}</td>"
            "<td>"
            f"<button data-action=\"resolve\" data-report-id=\"{html.escape(r.id)}\">Resolve</button>"
            f"<button data-action=\"dismiss\" data-report-id=\"{html.escape(r.id)}\">Dismiss</button>"
            "</td>"
            "</tr>"
        )
    return (
        '<table class="reports"><thead><tr>'
        "<th>Id</th><th>App</th><th>Reason</th><th>Reporter</th>"
        "<th>Filed</th><th>Actions</th></tr></thead><tbody>"
        + "".join(rows)
        + "</tbody></table>"
    )


def _render_apps(apps: list[AppEntry], allowed_tags: list[str]) -> str:
    if not apps:
        return '<p class="empty">No apps registered.</p>'
    rows = []
    for entry in apps:
        m = entry.manifest
        tag_checkboxes = []
        for tag in allowed_tags:
            checked = "checked" if tag in entry.tags else ""
            tag_checkboxes.append(
                f'<label><input type="checkbox" data-tag="{html.escape(tag)}" '
                f'data-app-id="{html.escape(m.id)}" {checked}> {html.escape(tag)}</label>'
            )
        rows.append(
            "<tr>"
            f"<td><code>{html.escape(m.id)}</code><br><small>{html.escape(m.name)}</small></td>"
            f"<td>{html.escape(m.url)}</td>"
            f"<td class=\"tags\">{''.join(tag_checkboxes)}</td>"
            f"<td>{html.escape(', '.join(m.categories))}</td>"
            "<td>"
            f"<button data-action=\"delete-app\" data-app-id=\"{html.escape(m.id)}\">Remove</button>"
            "</td>"
            "</tr>"
        )
    return (
        '<table class="apps"><thead><tr>'
        "<th>App</th><th>URL</th><th>Tags</th><th>Categories</th><th>Actions</th>"
        "</tr></thead><tbody>"
        + "".join(rows)
        + "</tbody></table>"
    )


def _render_log(entries: list[LogEntry]) -> str:
    if not entries:
        return '<p class="empty">No moderation actions yet.</p>'
    rows = []
    for e in entries:
        details_repr = ", ".join(
            f"{html.escape(k)}={html.escape(repr(v))}" for k, v in e.details.items()
        )
        rows.append(
            "<tr>"
            f"<td>{_fmt_ts(e.occurred_at)}</td>"
            f"<td><code>{html.escape(e.action)}</code></td>"
            f"<td>{html.escape(e.app_id or '—')}</td>"
            f"<td>{html.escape(e.actor)}</td>"
            f"<td><small>{details_repr or '—'}</small></td>"
            "</tr>"
        )
    return (
        '<table class="log"><thead><tr>'
        "<th>When</th><th>Action</th><th>App</th><th>Actor</th><th>Details</th>"
        "</tr></thead><tbody>"
        + "".join(rows)
        + "</tbody></table>"
    )


def _fmt_ts(ts: datetime) -> str:
    return html.escape(ts.isoformat(timespec="seconds"))


_PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>PagerOS Marketplace · Admin</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body {{ font: 14px/1.4 system-ui, sans-serif; margin: 1.5rem; color: #222; }}
  h1 {{ font-size: 1.4rem; margin: 0 0 0.5rem; }}
  h2 {{ font-size: 1.1rem; margin: 1.5rem 0 0.5rem; }}
  table {{ border-collapse: collapse; width: 100%; margin-bottom: 1rem; }}
  th, td {{ border: 1px solid #d0d7de; padding: 0.4rem 0.6rem; vertical-align: top; }}
  th {{ background: #f6f8fa; text-align: left; font-weight: 600; }}
  code {{ font: 12px/1.3 ui-monospace, SFMono-Regular, Menlo, monospace; }}
  .tags label {{ display: inline-block; margin-right: 0.7rem; }}
  button {{ font: inherit; padding: 0.2rem 0.6rem; cursor: pointer; }}
  button[data-action="delete-app"] {{ color: #b00020; }}
  .empty {{ color: #6e7781; font-style: italic; }}
  .topbar {{ display: flex; justify-content: space-between; align-items: baseline; }}
  .auth-hint {{ font-size: 12px; color: #6e7781; }}
  #flash {{ position: fixed; bottom: 1rem; right: 1rem; min-width: 240px; padding: 0.5rem 0.8rem;
           background: #1f2328; color: #fff; border-radius: 4px; opacity: 0; transition: opacity .2s;
           pointer-events: none; }}
  #flash.show {{ opacity: 0.95; }}
  #flash.err {{ background: #b00020; }}
</style>
</head>
<body>
<div class="topbar">
  <h1>PagerOS Marketplace · Admin</h1>
  <span class="auth-hint">
    Bearer token: <input type="password" id="admin-token" size="32" autocomplete="off">
  </span>
</div>

<h2>Open reports ({report_count})</h2>
{reports_section}

<h2>Registered apps ({app_count})</h2>
{apps_section}

<h2>Recent moderation log</h2>
{log_section}

<div id="flash"></div>

<script>
(function () {{
  const ALLOWED = {allowed_tags_json};
  const tokenInput = document.getElementById("admin-token");
  const flash = document.getElementById("flash");

  function notify(msg, isErr) {{
    flash.textContent = msg;
    flash.className = isErr ? "show err" : "show";
    setTimeout(() => flash.classList.remove("show"), 2400);
  }}

  async function api(method, path, body) {{
    const headers = {{}};
    const token = tokenInput.value.trim();
    if (token) headers["Authorization"] = "Bearer " + token;
    if (body !== undefined) headers["Content-Type"] = "application/json";
    const res = await fetch(path, {{
      method,
      headers,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    }});
    if (!res.ok) {{
      let detail = res.statusText;
      try {{ detail = (await res.json()).detail || detail; }} catch (e) {{}}
      throw new Error(detail);
    }}
    return res.status === 204 ? null : res.json();
  }}

  document.addEventListener("click", async (ev) => {{
    const btn = ev.target.closest("button[data-action]");
    if (!btn) return;
    const action = btn.dataset.action;
    try {{
      if (action === "resolve" || action === "dismiss") {{
        await api("POST", "/admin/reports/" + btn.dataset.reportId + "/" + action, {{}});
        notify("Report " + action + "d");
      }} else if (action === "delete-app") {{
        if (!confirm("Remove " + btn.dataset.appId + " from registry?")) return;
        await api("DELETE", "/apps/" + btn.dataset.appId);
        notify("App removed");
      }}
      setTimeout(() => location.reload(), 400);
    }} catch (err) {{
      notify(err.message, true);
    }}
  }});

  document.addEventListener("change", async (ev) => {{
    const cb = ev.target;
    if (!cb.matches("input[type=checkbox][data-tag]")) return;
    const appId = cb.dataset.appId;
    const tag = cb.dataset.tag;
    try {{
      if (cb.checked) {{
        await api("POST", "/admin/apps/" + appId + "/tags/" + tag);
        notify("Added '" + tag + "' to " + appId);
      }} else {{
        await api("DELETE", "/admin/apps/" + appId + "/tags/" + tag);
        notify("Removed '" + tag + "' from " + appId);
      }}
    }} catch (err) {{
      cb.checked = !cb.checked;  // revert
      notify(err.message, true);
    }}
  }});
}})();
</script>
</body>
</html>
"""
