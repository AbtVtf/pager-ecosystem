// Package simulate implements the `pagerctl simulate <url>` command.
//
// It validates a URL, locates the `pageros-simulator` binary, spawns it
// with `PAGEROS_SIMULATOR_URL=<url>` set (SIM-EXT-001 startup hook), and
// waits for it to exit. Signals received by the parent are forwarded to
// the child so Ctrl+C closes the simulator window cleanly.
package simulate

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"os/exec"
	"strings"
)

// SimulatorBinEnv lets users override the simulator binary path without
// touching PATH. Matches the convention used elsewhere in pagerctl
// (e.g. esptool location).
const SimulatorBinEnv = "PAGEROS_SIMULATOR_BIN"

// SimulatorURLEnv is the env var the simulator reads at startup
// (defined by SIM-EXT-001). Exported so `pagerctl dev` can reuse it.
const SimulatorURLEnv = "PAGEROS_SIMULATOR_URL"

// DefaultBin is the simulator binary name as installed by the SIM-008
// release bundles (deb/appimage/dmg/msi all install `pageros-simulator`).
const DefaultBin = "pageros-simulator"

// Options is the parsed CLI input for `pagerctl simulate`.
type Options struct {
	// URL is the target the simulator should render. Must be an absolute
	// http or https URL (positional argument).
	URL string
	// Bin overrides the simulator binary path. Empty means look up
	// SimulatorBinEnv, then PATH.
	Bin string
}

// Launcher spawns the simulator binary. Abstracted so tests can verify
// the env + args without actually running a GUI.
type Launcher interface {
	Launch(ctx context.Context, bin string, env []string, stdout, stderr io.Writer) error
}

// SystemLauncher runs the simulator as a real subprocess and waits for
// it to exit. Signals delivered to the parent (Ctrl+C / SIGTERM) are
// forwarded to the child by os/exec's default child-process group on
// Unix; on Windows the child receives CTRL_C via the shared console.
type SystemLauncher struct{}

// Launch execs `bin` with no extra args, passing through stdio and the
// supplied env. Blocks until the simulator exits.
func (SystemLauncher) Launch(ctx context.Context, bin string, env []string, stdout, stderr io.Writer) error {
	cmd := exec.CommandContext(ctx, bin)
	cmd.Env = env
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	cmd.Stdin = nil
	if err := cmd.Run(); err != nil {
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			return fmt.Errorf("simulator exited with status %d", exitErr.ExitCode())
		}
		return fmt.Errorf("simulator: %w", err)
	}
	return nil
}

// Run validates opts, resolves the simulator binary, and launches it
// pointed at opts.URL.
func Run(ctx context.Context, opts Options, launcher Launcher, stdout, stderr io.Writer) error {
	target, err := ValidateURL(opts.URL)
	if err != nil {
		return err
	}
	bin, err := ResolveBin(opts.Bin)
	if err != nil {
		return err
	}

	env := composeEnv(os.Environ(), target)

	fmt.Fprintf(stdout, "Opening simulator at %s (binary: %s)\n", target, bin)
	return launcher.Launch(ctx, bin, env, stdout, stderr)
}

// ValidateURL returns the canonical string form of an absolute http or
// https URL. PagerOS app servers speak HTTP only (SPEC.md §7.3); the
// simulator itself will surface any deeper transport error in its
// status bar, but we reject obviously wrong inputs here so the user
// gets a CLI error instead of a confusingly-empty simulator window.
func ValidateURL(raw string) (string, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "", errors.New("missing <url> argument")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("invalid url %q: %w", raw, err)
	}
	if !u.IsAbs() {
		return "", fmt.Errorf("invalid url %q: must be absolute (include scheme)", raw)
	}
	switch strings.ToLower(u.Scheme) {
	case "http", "https":
	default:
		return "", fmt.Errorf("invalid url %q: only http and https are supported (got %q)", raw, u.Scheme)
	}
	if u.Host == "" {
		return "", fmt.Errorf("invalid url %q: missing host", raw)
	}
	return u.String(), nil
}

// ResolveBin returns the path to the simulator binary, in priority
// order: explicit `--bin` override, then $PAGEROS_SIMULATOR_BIN, then
// PATH lookup for `pageros-simulator`.
func ResolveBin(override string) (string, error) {
	if override != "" {
		return override, nil
	}
	if env := strings.TrimSpace(os.Getenv(SimulatorBinEnv)); env != "" {
		return env, nil
	}
	bin, err := exec.LookPath(DefaultBin)
	if err != nil {
		return "", fmt.Errorf("simulator binary %q not found on PATH; install it (see simulator/RELEASING.md) or set %s", DefaultBin, SimulatorBinEnv)
	}
	return bin, nil
}

// composeEnv returns a copy of base with SimulatorURLEnv set to url,
// replacing any pre-existing entry for that key. Other vars (DISPLAY,
// HOME, etc.) are preserved so the simulator's webview can find the
// user's session resources.
func composeEnv(base []string, url string) []string {
	out := make([]string, 0, len(base)+1)
	prefix := SimulatorURLEnv + "="
	replaced := false
	for _, kv := range base {
		if strings.HasPrefix(kv, prefix) {
			out = append(out, prefix+url)
			replaced = true
			continue
		}
		out = append(out, kv)
	}
	if !replaced {
		out = append(out, prefix+url)
	}
	return out
}
