package simulate

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestValidateURL(t *testing.T) {
	cases := []struct {
		in   string
		want string
		err  bool
	}{
		{"http://localhost:8080/", "http://localhost:8080/", false},
		{"https://app.example.com/foo", "https://app.example.com/foo", false},
		{"  http://localhost:8080/  ", "http://localhost:8080/", false},
		{"HTTP://Localhost:8080/", "http://Localhost:8080/", false}, // scheme normalized; host preserved
		{"", "", true},
		{"   ", "", true},
		{"localhost:8080", "", true}, // not absolute
		{"/foo", "", true},
		{"ftp://example.com/", "", true},
		{"http:///", "", true}, // missing host
	}
	for _, c := range cases {
		t.Run(c.in, func(t *testing.T) {
			got, err := ValidateURL(c.in)
			if c.err {
				if err == nil {
					t.Fatalf("expected error for %q, got %q", c.in, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error for %q: %v", c.in, err)
			}
			if got != c.want {
				t.Errorf("ValidateURL(%q) = %q, want %q", c.in, got, c.want)
			}
		})
	}
}

func TestResolveBin_Override(t *testing.T) {
	t.Setenv(SimulatorBinEnv, "")
	bin, err := ResolveBin("/custom/path/sim")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if bin != "/custom/path/sim" {
		t.Errorf("got %q, want explicit override", bin)
	}
}

func TestResolveBin_EnvVar(t *testing.T) {
	t.Setenv(SimulatorBinEnv, "/from/env/sim")
	bin, err := ResolveBin("")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if bin != "/from/env/sim" {
		t.Errorf("got %q, want env override", bin)
	}
}

func TestResolveBin_EnvOverridesPath(t *testing.T) {
	// Even if the binary exists on PATH, an explicit env var wins so
	// engineers can point at a debug build.
	t.Setenv(SimulatorBinEnv, "/explicit/wins")
	// Make sure a real binary is on PATH too (the test binary itself).
	if runtime.GOOS != "windows" {
		t.Setenv("PATH", filepath.Dir(os.Args[0])+":"+os.Getenv("PATH"))
	}
	bin, err := ResolveBin("")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if bin != "/explicit/wins" {
		t.Errorf("env var did not win over PATH: got %q", bin)
	}
}

func TestResolveBin_NotFound(t *testing.T) {
	t.Setenv(SimulatorBinEnv, "")
	t.Setenv("PATH", t.TempDir()) // guaranteed to lack the binary
	_, err := ResolveBin("")
	if err == nil {
		t.Fatal("expected error when simulator binary missing")
	}
	if !strings.Contains(err.Error(), DefaultBin) {
		t.Errorf("error should mention binary name, got %q", err.Error())
	}
	if !strings.Contains(err.Error(), SimulatorBinEnv) {
		t.Errorf("error should mention env-var override, got %q", err.Error())
	}
}

func TestComposeEnv_Adds(t *testing.T) {
	base := []string{"HOME=/h", "PATH=/p"}
	out := composeEnv(base, "http://x/")
	want := []string{"HOME=/h", "PATH=/p", SimulatorURLEnv + "=http://x/"}
	if !equalSlice(out, want) {
		t.Errorf("got %v, want %v", out, want)
	}
}

func TestComposeEnv_Replaces(t *testing.T) {
	base := []string{"HOME=/h", SimulatorURLEnv + "=http://old/", "PATH=/p"}
	out := composeEnv(base, "http://new/")
	want := []string{"HOME=/h", SimulatorURLEnv + "=http://new/", "PATH=/p"}
	if !equalSlice(out, want) {
		t.Errorf("got %v, want %v", out, want)
	}
}

func TestComposeEnv_PreservesBase(t *testing.T) {
	base := []string{"HOME=/h"}
	_ = composeEnv(base, "http://x/")
	// mutating output must not mutate caller's slice
	if len(base) != 1 || base[0] != "HOME=/h" {
		t.Errorf("base mutated: %v", base)
	}
}

func TestRun_LaunchesWithEnvAndBin(t *testing.T) {
	fl := &fakeLauncher{}
	var stdout, stderr bytes.Buffer
	err := Run(context.Background(),
		Options{URL: "http://127.0.0.1:8080/", Bin: "/fake/sim"},
		fl, &stdout, &stderr)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if fl.bin != "/fake/sim" {
		t.Errorf("bin = %q, want /fake/sim", fl.bin)
	}
	if !containsKV(fl.env, SimulatorURLEnv+"=http://127.0.0.1:8080/") {
		t.Errorf("env missing %s entry: %v", SimulatorURLEnv, fl.env)
	}
	if !strings.Contains(stdout.String(), "http://127.0.0.1:8080/") {
		t.Errorf("stdout should report target URL: %q", stdout.String())
	}
	if !strings.Contains(stdout.String(), "/fake/sim") {
		t.Errorf("stdout should report binary: %q", stdout.String())
	}
}

func TestRun_RejectsBadURL(t *testing.T) {
	fl := &fakeLauncher{}
	err := Run(context.Background(),
		Options{URL: "notaurl", Bin: "/fake/sim"},
		fl, io.Discard, io.Discard)
	if err == nil {
		t.Fatal("expected URL validation error")
	}
	if fl.bin != "" {
		t.Errorf("launcher should not be invoked on bad URL, got bin=%q", fl.bin)
	}
}

func TestRun_PropagatesBinResolveError(t *testing.T) {
	t.Setenv(SimulatorBinEnv, "")
	t.Setenv("PATH", t.TempDir())
	fl := &fakeLauncher{}
	err := Run(context.Background(),
		Options{URL: "http://x/", Bin: ""},
		fl, io.Discard, io.Discard)
	if err == nil {
		t.Fatal("expected resolve error")
	}
	if fl.bin != "" {
		t.Errorf("launcher should not be invoked: %q", fl.bin)
	}
}

type fakeLauncher struct {
	bin string
	env []string
}

func (f *fakeLauncher) Launch(ctx context.Context, bin string, env []string, stdout, stderr io.Writer) error {
	f.bin = bin
	f.env = env
	return nil
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

func containsKV(env []string, want string) bool {
	for _, kv := range env {
		if kv == want {
			return true
		}
	}
	return false
}
