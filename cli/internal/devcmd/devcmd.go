// Package devcmd implements the `pagerctl dev` command.
//
// `pagerctl dev` spins up the user's app server with auto-reload (via the
// Python SDK's `pageros.devserver` module, PY-009) and opens the simulator
// pointed at it (via SIM-EXT-001's PAGEROS_SIMULATOR_URL contract). One
// command replaces the previous two-step "run python, then open the sim
// and Alt+G a URL" loop documented in `cli/internal/initapp/templates`.
//
// File-change reload is owned by `pageros.devserver`; this package's job
// is process orchestration: start dev server → wait for its port →
// start simulator → propagate Ctrl+C to both → exit when either exits.
package devcmd

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/pageros/pagerctl/internal/simulate"
)

// PythonEnv lets users pin the Python interpreter (e.g. a venv's python).
// Matches the convention from `simulate` package (PAGEROS_SIMULATOR_BIN).
const PythonEnv = "PAGEROS_PYTHON"

// DefaultPython is the Python binary name used when neither --python nor
// PAGEROS_PYTHON is set. `python3` is the de-facto current name on Linux
// and macOS; on Windows, users typically have `python.exe` which is
// auto-resolved by exec.LookPath via PATHEXT.
const DefaultPython = "python3"

// DefaultApp is the entry point pagerctl init scaffolds.
const DefaultApp = "app.py"

// DefaultHost is the loopback address the dev server binds by default.
const DefaultHost = "127.0.0.1"

// DefaultPort is the dev server's default port (matches the SDK's
// `App.run` default).
const DefaultPort = 8000

// PortWaitTimeout caps how long we wait for the dev server to start
// listening before launching the simulator. The dev server's cold start
// (Python import + HTTP bind) is typically <500 ms; 10 s is a generous
// ceiling so a slow venv import doesn't make the sim race the server.
const PortWaitTimeout = 10 * time.Second

// PortWaitInterval is how often we re-probe the dev server port.
const PortWaitInterval = 50 * time.Millisecond

// Options is the parsed CLI input for `pagerctl dev`.
type Options struct {
	App          string // path to user's app entry (default: app.py in cwd)
	Host         string // dev server bind host
	Port         int    // dev server bind port
	Python       string // interpreter (default: PAGEROS_PYTHON, then python3)
	SimulatorBin string // forwarded to internal/simulate (PAGEROS_SIMULATOR_BIN | --bin)
	// NoSimulator is an escape hatch for users who want only the dev
	// server (e.g. they'll connect a real device). Defaults to false so
	// the acceptance criterion "opens the simulator pointing at it" is
	// met by the no-flag invocation.
	NoSimulator bool
}

// Runner abstracts subprocess spawning so tests can verify the wiring
// without running real Python / Tauri processes.
type Runner interface {
	// StartDevServer launches the auto-reloading app server. Returns a
	// Process whose Wait() blocks until the server exits.
	StartDevServer(ctx context.Context, python, appPath, host string, port int, stdout, stderr io.Writer) (Process, error)
	// StartSimulator launches the simulator pointed at url. Returns a
	// Process whose Wait() blocks until the window closes.
	StartSimulator(ctx context.Context, simBin, url string, stdout, stderr io.Writer) (Process, error)
	// WaitForPort polls host:port until something accepts a TCP
	// connection or the deadline expires.
	WaitForPort(ctx context.Context, host string, port int, timeout time.Duration) error
}

// Process is the slice of os/exec.Cmd the orchestrator needs.
type Process interface {
	Wait() error
}

// SystemRunner is the production Runner: real os/exec subprocesses, real
// TCP probes.
type SystemRunner struct{}

// StartDevServer execs `python -m pageros.devserver --app … --host … --port …`.
// `-u` is set so child stdout/stderr aren't line-buffered, matching the
// dev server's default subprocess command for the app itself.
func (SystemRunner) StartDevServer(ctx context.Context, python, appPath, host string, port int, stdout, stderr io.Writer) (Process, error) {
	cmd := exec.CommandContext(ctx, python, "-u", "-m", "pageros.devserver",
		"--app", appPath, "--host", host, "--port", strconv.Itoa(port))
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	cmd.Stdin = nil
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start dev server (%s): %w", python, err)
	}
	return cmd, nil
}

// StartSimulator delegates to `internal/simulate.SystemLauncher` so the
// env-var contract and exit-status handling stay in one place.
func (SystemRunner) StartSimulator(ctx context.Context, simBin, url string, stdout, stderr io.Writer) (Process, error) {
	bin, err := simulate.ResolveBin(simBin)
	if err != nil {
		return nil, err
	}
	env := os.Environ()
	env = setEnv(env, simulate.SimulatorURLEnv, url)
	cmd := exec.CommandContext(ctx, bin)
	cmd.Env = env
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	cmd.Stdin = nil
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start simulator (%s): %w", bin, err)
	}
	return cmd, nil
}

// WaitForPort polls host:port until a connection succeeds, ctx is
// cancelled, or timeout elapses. Returns the last connection error on
// timeout so the caller can include it in a user-visible message.
func (SystemRunner) WaitForPort(ctx context.Context, host string, port int, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	addr := net.JoinHostPort(host, strconv.Itoa(port))
	var lastErr error
	for {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		conn, err := net.DialTimeout("tcp", addr, 500*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return nil
		}
		lastErr = err
		if time.Now().After(deadline) {
			return fmt.Errorf("timed out after %s waiting for %s: %w", timeout, addr, lastErr)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(PortWaitInterval):
		}
	}
}

// Run orchestrates the dev server + simulator subprocesses. Blocks until
// either subprocess exits or the parent context is cancelled (e.g. Ctrl+C).
//
// Exit semantics:
//   - dev server exits → cancel simulator, return server's error
//   - simulator exits  → cancel dev server, return nil (user closed the window)
//   - parent ctx cancelled → cancel both, return ctx.Err()
func Run(ctx context.Context, opts Options, runner Runner, stdout, stderr io.Writer) error {
	opts, err := opts.Defaults()
	if err != nil {
		return err
	}
	if _, err := os.Stat(opts.App); err != nil {
		return fmt.Errorf("app entry %q: %w", opts.App, err)
	}

	url := fmt.Sprintf("http://%s/", net.JoinHostPort(opts.Host, strconv.Itoa(opts.Port)))

	runCtx, cancel := context.WithCancel(ctx)
	defer cancel()

	fmt.Fprintf(stdout, "Starting dev server: %s -m pageros.devserver --app %s --host %s --port %d\n",
		opts.Python, opts.App, opts.Host, opts.Port)
	server, err := runner.StartDevServer(runCtx, opts.Python, opts.App, opts.Host, opts.Port, stdout, stderr)
	if err != nil {
		return err
	}
	serverDone := waitAsync(server)

	if opts.NoSimulator {
		fmt.Fprintf(stdout, "Dev server running (--no-simulator). Press Ctrl+C to stop.\n")
		return blockOn(ctx, cancel, serverDone, nil)
	}

	fmt.Fprintf(stdout, "Waiting for dev server on %s (up to %s)...\n",
		net.JoinHostPort(opts.Host, strconv.Itoa(opts.Port)), PortWaitTimeout)
	if err := runner.WaitForPort(runCtx, opts.Host, opts.Port, PortWaitTimeout); err != nil {
		cancel()
		// Drain server before reporting so its stderr surfaces first.
		<-serverDone
		return fmt.Errorf("dev server did not start: %w", err)
	}

	fmt.Fprintf(stdout, "Opening simulator at %s\n", url)
	sim, err := runner.StartSimulator(runCtx, opts.SimulatorBin, url, stdout, stderr)
	if err != nil {
		cancel()
		<-serverDone
		return err
	}
	simDone := waitAsync(sim)

	return blockOn(ctx, cancel, serverDone, simDone)
}

// Defaults fills in unset fields. Returns an error only if a supplied
// value is malformed.
func (o Options) Defaults() (Options, error) {
	if o.App == "" {
		o.App = DefaultApp
	}
	abs, err := filepath.Abs(o.App)
	if err != nil {
		return o, fmt.Errorf("resolve app path %q: %w", o.App, err)
	}
	o.App = abs
	if o.Host == "" {
		o.Host = DefaultHost
	}
	if o.Port == 0 {
		o.Port = DefaultPort
	}
	if o.Port < 1 || o.Port > 65535 {
		return o, fmt.Errorf("invalid --port %d (must be 1-65535)", o.Port)
	}
	if o.Python == "" {
		if env := strings.TrimSpace(os.Getenv(PythonEnv)); env != "" {
			o.Python = env
		} else {
			o.Python = DefaultPython
		}
	}
	return o, nil
}

// blockOn waits for the first subprocess to finish, then cancels the
// run context and drains the other. Returns the server's error if the
// server exited first, nil if the simulator did or the parent was
// cancelled (Ctrl+C). simDone may be nil when --no-simulator.
func blockOn(parentCtx context.Context, cancel context.CancelFunc, serverDone, simDone <-chan error) error {
	var (
		serverErr error
		serverHit bool
		simErr    error
		simHit    bool
		parentHit bool
	)

	select {
	case err := <-serverDone:
		serverErr, serverHit = err, true
	case err := <-receiveOrNever(simDone):
		simErr, simHit = err, true
	case <-parentCtx.Done():
		parentHit = true
	}
	cancel()

	// Drain whichever channel hasn't fired yet so subprocesses are
	// fully reaped before Run returns. Surfaced errors from drains are
	// discarded — they're almost always "signal: killed" from cancel.
	if !serverHit {
		<-serverDone
	}
	if simDone != nil && !simHit {
		<-simDone
	}

	switch {
	case parentHit:
		return nil
	case serverHit && serverErr != nil && !isKilledBySignal(serverErr):
		return fmt.Errorf("dev server: %w", serverErr)
	case simHit && simErr != nil && !isKilledBySignal(simErr):
		// Simulator exit isn't fatal (user closed the window) — only
		// surface if it crashed with a non-signal error.
		return fmt.Errorf("simulator: %w", simErr)
	}
	return nil
}

func waitAsync(p Process) <-chan error {
	ch := make(chan error, 1)
	go func() {
		ch <- p.Wait()
		close(ch)
	}()
	return ch
}

// receiveOrNever returns a channel that fires only if ch is non-nil,
// so a nil simDone in the select arm never wins.
func receiveOrNever(ch <-chan error) <-chan error {
	if ch != nil {
		return ch
	}
	return nil
}

// isKilledBySignal returns true when a subprocess was terminated by a
// signal rather than exiting on its own (Unix ExitCode == -1). Used to
// suppress noisy "dev server: signal: killed" errors when we cancelled
// the child ourselves.
func isKilledBySignal(err error) bool {
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return exitErr.ExitCode() < 0
	}
	return false
}

// setEnv returns base with key=value set, replacing any prior entry.
func setEnv(base []string, key, value string) []string {
	out := make([]string, 0, len(base)+1)
	prefix := key + "="
	replaced := false
	for _, kv := range base {
		if strings.HasPrefix(kv, prefix) {
			out = append(out, prefix+value)
			replaced = true
			continue
		}
		out = append(out, kv)
	}
	if !replaced {
		out = append(out, prefix+value)
	}
	return out
}
