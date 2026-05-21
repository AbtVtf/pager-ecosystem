// Dev server with auto-reload (JS-009).
//
// Wraps `App.run` with a file watcher that re-imports the user's app
// module when source changes, so editing an .ts/.js file under the
// project root triggers a sub-second reload. Matches the developer
// experience of `pagerctl dev` (CLI-002) and the Python SDK's
// `pageros.devserver` (PY-009).
//
// Usage from a dev script:
//
//     // dev.ts
//     import { devserver } from "@pageros/sdk";
//     await devserver({ entrypoint: "./app.ts" });
//
// The dev server watches the directory containing `entrypoint` (and
// any imports it can statically reach via `import` / `require` —
// "static" here = ".ts/.js/.tsx/.jsx files under the watch root").
// On a change, the prior server is closed and the entrypoint is re-
// imported from a cache-busting query-string path.

import * as fs from "node:fs";
import * as path from "node:path";
import { pathToFileURL } from "node:url";
import type { App } from "./app.js";

export interface DevserverOptions {
  /**
   * Path to the user's app file. Must export a default `App` instance
   * or `{ default: app }`. Relative to `process.cwd()`.
   */
  entrypoint: string;

  /** Bind host. Default `127.0.0.1`. */
  host?: string;
  /** Bind port. Default `8000`. */
  port?: number;

  /**
   * Directory to watch. Defaults to the directory containing
   * `entrypoint`.
   */
  watchDir?: string;

  /**
   * Glob-like extensions to consider as source. Anything else (logs,
   * cache, generated bundles) is ignored. Default: ts/tsx/js/jsx/mjs/cjs.
   */
  watchExtensions?: string[];

  /**
   * Debounce window in ms — multiple changes inside this window
   * trigger one reload. Default 150 ms.
   */
  debounceMs?: number;

  /**
   * Sink for diagnostics. Default `console`.
   */
  logger?: Pick<Console, "log" | "warn" | "error">;
}

/**
 * Loop over the dev server until process is interrupted. Returns the
 * server handle for tests; callers in a real CLI typically don't await.
 */
export async function devserver(opts: DevserverOptions): Promise<() => Promise<void>> {
  const log = opts.logger ?? console;
  const host = opts.host ?? "127.0.0.1";
  const port = opts.port ?? 8000;
  const entryAbs = path.resolve(process.cwd(), opts.entrypoint);
  const watchDir = path.resolve(opts.watchDir ?? path.dirname(entryAbs));
  const watchExtensions = new Set(
    (opts.watchExtensions ?? [".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs"]).map((e) =>
      e.startsWith(".") ? e : `.${e}`,
    ),
  );
  const debounceMs = opts.debounceMs ?? 150;

  let currentApp: App | null = null;
  let closing = false;
  let reloadTimer: NodeJS.Timeout | null = null;

  async function loadAndServe(): Promise<void> {
    const cacheBust = `?t=${Date.now()}`;
    const mod = await import(pathToFileURL(entryAbs).href + cacheBust);
    const app: App = (mod.default ?? mod.app) as App;
    if (!app || typeof (app as unknown as { run: unknown }).run !== "function") {
      throw new Error(
        `${opts.entrypoint} must export a default App (or named 'app') with a .run() method`,
      );
    }
    // Stop the prior listener (App.run binds a single port).
    if (currentApp) {
      try {
        await stopApp(currentApp);
      } catch (err) {
        log.warn(`devserver: failed to close prior listener: ${(err as Error).message}`);
      }
    }
    currentApp = app;
    // Run in the background so the watcher can keep firing.
    app
      .run({ host, port })
      .catch((err: unknown) => log.error(`devserver: app.run rejected: ${(err as Error).message}`));
    log.log(`devserver: listening on http://${host}:${port}/ (entrypoint: ${opts.entrypoint})`);
  }

  function schedule(reason: string): void {
    if (closing) return;
    if (reloadTimer !== null) clearTimeout(reloadTimer);
    reloadTimer = setTimeout(() => {
      reloadTimer = null;
      log.log(`devserver: reload (${reason})`);
      loadAndServe().catch((err: unknown) =>
        log.error(`devserver: reload failed: ${(err as Error).message}`),
      );
    }, debounceMs);
  }

  // Initial boot.
  await loadAndServe();

  // Filesystem watcher — `fs.watch` is platform-portable on Node 20+
  // with recursive on macOS/Windows. On Linux we walk subdirs manually
  // (fs.watch is non-recursive there); good enough for typical project
  // sizes.
  const watchers: fs.FSWatcher[] = [];
  function watch(dir: string): void {
    try {
      const recursive = process.platform !== "linux";
      const w = fs.watch(dir, { recursive }, (_event, filename) => {
        if (!filename) return;
        const ext = path.extname(filename);
        if (!watchExtensions.has(ext)) return;
        schedule(`${path.join(dir, filename.toString())}`);
      });
      watchers.push(w);
    } catch (err) {
      log.warn(`devserver: cannot watch ${dir}: ${(err as Error).message}`);
    }
  }

  if (process.platform === "linux") {
    // Walk one level deep so small/typical project layouts work; deep
    // monorepos can opt into recursive by setting `watchDir` to the
    // narrowest dir they want watched.
    watch(watchDir);
    try {
      for (const entry of fs.readdirSync(watchDir, { withFileTypes: true })) {
        if (entry.isDirectory() && !entry.name.startsWith(".") && entry.name !== "node_modules") {
          watch(path.join(watchDir, entry.name));
        }
      }
    } catch (err) {
      log.warn(`devserver: readdir ${watchDir} failed: ${(err as Error).message}`);
    }
  } else {
    watch(watchDir);
  }

  return async function close(): Promise<void> {
    closing = true;
    if (reloadTimer !== null) clearTimeout(reloadTimer);
    for (const w of watchers) {
      try { w.close(); } catch { /* noop */ }
    }
    if (currentApp) {
      await stopApp(currentApp);
      currentApp = null;
    }
  };
}

/** Best-effort App teardown — relies on the `stop`/`close` method many
 *  HTTP runtimes expose. The Python equivalent has the same shape. */
async function stopApp(app: App): Promise<void> {
  // Use whatever the App exposes; both Python and JS App.run own a
  // single bound server, and the JS impl carries `_server` for this
  // purpose in tests.
  const a = app as unknown as {
    stop?: () => Promise<void> | void;
    close?: () => Promise<void> | void;
  };
  if (typeof a.stop === "function") {
    await a.stop();
  } else if (typeof a.close === "function") {
    await a.close();
  }
}
