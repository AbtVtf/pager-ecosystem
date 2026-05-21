package devcmd

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestDefaults_FillsAll(t *testing.T) {
	t.Setenv(PythonEnv, "")
	tmpApp := writeTempApp(t)
	o := Options{App: tmpApp}
	got, err := o.Defaults()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Host != DefaultHost {
		t.Errorf("Host = %q, want %q", got.Host, DefaultHost)
	}
	if got.Port != DefaultPort {
		t.Errorf("Port = %d, want %d", got.Port, DefaultPort)
	}
	if got.Python != DefaultPython {
		t.Errorf("Python = %q, want %q", got.Python, DefaultPython)
	}
	if !filepath.IsAbs(got.App) {
		t.Errorf("App path should be absolute, got %q", got.App)
	}
}

func TestDefaults_EnvPython(t *testing.T) {
	t.Setenv(PythonEnv, "/venv/bin/python")
	o, err := Options{App: "app.py"}.Defaults()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if o.Python != "/venv/bin/python" {
		t.Errorf("Python = %q, want env override", o.Python)
	}
}

func TestDefaults_ExplicitPythonBeatsEnv(t *testing.T) {
	t.Setenv(PythonEnv, "/venv/bin/python")
	o, err := Options{App: "app.py", Python: "/explicit/python"}.Defaults()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if o.Python != "/explicit/python" {
		t.Errorf("Python = %q, want explicit override", o.Python)
	}
}

func TestDefaults_RejectsBadPort(t *testing.T) {
	_, err := Options{App: "app.py", Port: 99999}.Defaults()
	if err == nil {
		t.Fatal("expected error for out-of-range port")
	}
}

func TestRun_MissingAppFile(t *testing.T) {
	err := Run(context.Background(),
		Options{App: filepath.Join(t.TempDir(), "nope.py")},
		&fakeRunner{}, io.Discard, io.Discard)
	if err == nil {
		t.Fatal("expected error for missing app file")
	}
}

func TestRun_NoSimulator_BlocksUntilServerExits(t *testing.T) {
	app := writeTempApp(t)
	server := &fakeProcess{exit: make(chan error, 1)}
	r := &fakeRunner{server: server}
	doneCh := make(chan error, 1)
	go func() {
		doneCh <- Run(context.Background(),
			Options{App: app, NoSimulator: true},
			r, io.Discard, io.Discard)
	}()
	// Verify the simulator was never asked to start.
	time.Sleep(20 * time.Millisecond)
	if r.simStarted.Load() {
		t.Fatal("simulator should not start when --no-simulator")
	}
	// Cleanly exit the dev server.
	server.exit <- nil
	if err := waitFor(doneCh); err != nil {
		t.Fatalf("Run returned: %v", err)
	}
}

func TestRun_FullPipeline(t *testing.T) {
	app := writeTempApp(t)
	server := &fakeProcess{exit: make(chan error, 1)}
	sim := &fakeProcess{exit: make(chan error, 1)}
	r := &fakeRunner{server: server, sim: sim}

	var stdout bytes.Buffer
	doneCh := make(chan error, 1)
	go func() {
		doneCh <- Run(context.Background(),
			Options{App: app},
			r, &stdout, io.Discard)
	}()

	// Wait for sim to be requested.
	if err := waitTrue(&r.simStarted, time.Second); err != nil {
		t.Fatalf("sim never started: %v", err)
	}
	if got := r.simURL.Load(); got == nil || !strings.HasPrefix(got.(string), "http://") {
		t.Errorf("sim URL = %v, want http://… form", got)
	}
	// User closes the simulator window (clean exit).
	sim.exit <- nil
	if err := waitFor(doneCh); err != nil {
		t.Fatalf("Run returned: %v", err)
	}
	if !strings.Contains(stdout.String(), "Opening simulator at http://127.0.0.1:8000/") {
		t.Errorf("stdout missing simulator URL line:\n%s", stdout.String())
	}
}

func TestRun_PortWaitTimeoutSurfacesError(t *testing.T) {
	app := writeTempApp(t)
	server := &fakeProcess{exit: make(chan error, 1)}
	r := &fakeRunner{server: server, portErr: errors.New("connection refused")}

	doneCh := make(chan error, 1)
	go func() {
		doneCh <- Run(context.Background(),
			Options{App: app},
			r, io.Discard, io.Discard)
	}()
	// fakeRunner's ctx-done goroutine pushes a synthetic exit error
	// when the run context is cancelled, so the drain in Run() returns.
	err := waitFor(doneCh)
	if err == nil {
		t.Fatal("expected error when dev server fails to listen")
	}
	if !strings.Contains(err.Error(), "dev server did not start") {
		t.Errorf("error should mention startup failure, got %q", err.Error())
	}
}

func TestRun_ServerCrashIsReported(t *testing.T) {
	app := writeTempApp(t)
	// Server "exits" with a real exit error before sim ever starts.
	server := &fakeProcess{exit: make(chan error, 1)}
	r := &fakeRunner{server: server, sim: &fakeProcess{exit: make(chan error, 1)}}

	doneCh := make(chan error, 1)
	go func() {
		doneCh <- Run(context.Background(),
			Options{App: app},
			r, io.Discard, io.Discard)
	}()
	// Wait for sim to be started so we know we're past the port-wait.
	_ = waitTrue(&r.simStarted, time.Second)
	server.exit <- &fakeExitErr{code: 2}
	// fakeRunner cancels sim via ctx-done; no manual push needed.
	err := waitFor(doneCh)
	if err == nil {
		t.Fatal("expected dev-server error to surface")
	}
	if !strings.Contains(err.Error(), "dev server:") {
		t.Errorf("error should be prefixed with 'dev server:': %q", err.Error())
	}
}

func TestRun_ParentCtxCancelIsSilent(t *testing.T) {
	app := writeTempApp(t)
	server := &fakeProcess{exit: make(chan error, 1)}
	sim := &fakeProcess{exit: make(chan error, 1)}
	r := &fakeRunner{server: server, sim: sim}

	ctx, cancel := context.WithCancel(context.Background())
	doneCh := make(chan error, 1)
	go func() {
		doneCh <- Run(ctx, Options{App: app}, r, io.Discard, io.Discard)
	}()
	_ = waitTrue(&r.simStarted, time.Second)
	cancel()
	// fakeRunner cancels both via ctx-done goroutines on cancel.
	if err := waitFor(doneCh); err != nil {
		t.Fatalf("Ctrl+C path should return nil, got %v", err)
	}
}

func TestSetEnv_AddsAndReplaces(t *testing.T) {
	base := []string{"FOO=1"}
	out := setEnv(base, "BAR", "2")
	if !equalSlice(out, []string{"FOO=1", "BAR=2"}) {
		t.Errorf("add: got %v", out)
	}
	out = setEnv([]string{"FOO=1", "BAR=2"}, "FOO", "x")
	if !equalSlice(out, []string{"FOO=x", "BAR=2"}) {
		t.Errorf("replace: got %v", out)
	}
}

// SystemRunner.WaitForPort is the one bit of SystemRunner worth testing
// directly — it touches the network. Use a transient listener to exercise
// both the success and timeout paths.
func TestSystemRunner_WaitForPort_Success(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()
	host, portStr, _ := net.SplitHostPort(ln.Addr().String())
	port := atoi(t, portStr)

	r := SystemRunner{}
	if err := r.WaitForPort(context.Background(), host, port, 2*time.Second); err != nil {
		t.Fatalf("WaitForPort returned: %v", err)
	}
}

func TestSystemRunner_WaitForPort_Timeout(t *testing.T) {
	// Pick a port that nothing is listening on by binding briefly then
	// closing the listener, then probing it.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	host, portStr, _ := net.SplitHostPort(ln.Addr().String())
	port := atoi(t, portStr)
	ln.Close()

	if runtime.GOOS == "windows" {
		t.Skip("port reuse semantics on Windows make this timing flaky")
	}

	r := SystemRunner{}
	err = r.WaitForPort(context.Background(), host, port, 250*time.Millisecond)
	if err == nil {
		t.Fatal("expected timeout error")
	}
	if !strings.Contains(err.Error(), "timed out") {
		t.Errorf("error should be timeout-shaped: %q", err.Error())
	}
}

// ─── helpers ─────────────────────────────────────────────────────────────

type fakeRunner struct {
	server     *fakeProcess
	sim        *fakeProcess
	portErr    error
	simStarted atomicBool
	simURL     atomicAny
}

func (f *fakeRunner) StartDevServer(ctx context.Context, python, appPath, host string, port int, stdout, stderr io.Writer) (Process, error) {
	if f.server == nil {
		return nil, errors.New("no fake server configured")
	}
	go func() {
		<-ctx.Done()
		// Ensure ctx-cancel drains the exit channel even if the test
		// didn't push an error; closed processes look like ExitCode<0.
		select {
		case f.server.exit <- &fakeExitErr{code: -1}:
		default:
		}
	}()
	return f.server, nil
}

func (f *fakeRunner) StartSimulator(ctx context.Context, simBin, url string, stdout, stderr io.Writer) (Process, error) {
	f.simStarted.Store(true)
	f.simURL.Store(url)
	if f.sim == nil {
		return nil, errors.New("no fake sim configured")
	}
	go func() {
		<-ctx.Done()
		select {
		case f.sim.exit <- &fakeExitErr{code: -1}:
		default:
		}
	}()
	return f.sim, nil
}

func (f *fakeRunner) WaitForPort(ctx context.Context, host string, port int, timeout time.Duration) error {
	return f.portErr
}

type fakeProcess struct {
	exit chan error
	once sync.Once
	err  error
}

func (p *fakeProcess) Wait() error {
	p.once.Do(func() {
		p.err = <-p.exit
	})
	return p.err
}

type fakeExitErr struct{ code int }

func (e *fakeExitErr) Error() string { return fmt.Sprintf("exit %d", e.code) }
func (e *fakeExitErr) ExitCode() int { return e.code }

type atomicBool struct {
	mu sync.Mutex
	v  bool
}

func (a *atomicBool) Store(v bool) { a.mu.Lock(); defer a.mu.Unlock(); a.v = v }
func (a *atomicBool) Load() bool   { a.mu.Lock(); defer a.mu.Unlock(); return a.v }

type atomicAny struct {
	mu sync.Mutex
	v  any
}

func (a *atomicAny) Store(v any) { a.mu.Lock(); defer a.mu.Unlock(); a.v = v }
func (a *atomicAny) Load() any   { a.mu.Lock(); defer a.mu.Unlock(); return a.v }

func writeTempApp(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "app.py")
	if err := os.WriteFile(path, []byte("# fake app\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func waitFor(ch <-chan error) error {
	select {
	case err := <-ch:
		return err
	case <-time.After(3 * time.Second):
		return errors.New("waitFor timed out")
	}
}

func waitTrue(b *atomicBool, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if b.Load() {
			return nil
		}
		time.Sleep(5 * time.Millisecond)
	}
	return errors.New("condition never true")
}

func equalSlice(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func atoi(t *testing.T, s string) int {
	t.Helper()
	n := 0
	for _, c := range s {
		if c < '0' || c > '9' {
			t.Fatalf("not a number: %q", s)
		}
		n = n*10 + int(c-'0')
	}
	return n
}
