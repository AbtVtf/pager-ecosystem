package initapp

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestRun_PythonScaffold(t *testing.T) {
	dir := t.TempDir()
	opts := Options{
		Lang: "python",
		Name: "demo",
		Dir:  dir,
	}
	var out bytes.Buffer
	res, err := Run(opts, &out)
	if err != nil {
		t.Fatalf("Run: %v\n%s", err, out.String())
	}
	if res.TargetDir != filepath.Join(dir, "demo") {
		t.Errorf("TargetDir = %q", res.TargetDir)
	}
	wantFiles := []string{"app.py", "manifest.yaml", "requirements.txt", "README.md", ".gitignore"}
	for _, f := range wantFiles {
		path := filepath.Join(res.TargetDir, f)
		if _, err := os.Stat(path); err != nil {
			t.Errorf("expected %s: %v", path, err)
		}
	}
	appBody, err := os.ReadFile(filepath.Join(res.TargetDir, "app.py"))
	if err != nil {
		t.Fatalf("read app.py: %v", err)
	}
	body := string(appBody)
	if !strings.Contains(body, `App(name="demo")`) {
		t.Errorf("app.py should use the project name. got:\n%s", body)
	}
	if !strings.Contains(body, `from pageros import App`) {
		t.Errorf("app.py should import pageros.App. got:\n%s", body)
	}
	// Acceptance: minimal hello-world ≤ 20 LOC (counting non-empty, non-comment lines).
	loc := 0
	for _, line := range strings.Split(body, "\n") {
		s := strings.TrimSpace(line)
		if s == "" || strings.HasPrefix(s, "#") || strings.HasPrefix(s, `"""`) {
			continue
		}
		loc++
	}
	if loc > 20 {
		t.Errorf("hello-world app.py is %d LOC, want ≤ 20", loc)
	}

	manifestBody, _ := os.ReadFile(filepath.Join(res.TargetDir, "manifest.yaml"))
	if !strings.Contains(string(manifestBody), "id: demo.example.com") {
		t.Errorf("manifest should default app id to demo.example.com. got:\n%s", manifestBody)
	}
}

func TestRun_JSReportsNotYet(t *testing.T) {
	dir := t.TempDir()
	opts := Options{
		Lang: "js",
		Name: "demo",
		Dir:  dir,
	}
	_, err := Run(opts, &bytes.Buffer{})
	if err == nil {
		t.Fatal("expected error for JS")
	}
	if !strings.Contains(err.Error(), "JS-001") {
		t.Errorf("error should point at JS-001: %v", err)
	}
	if _, statErr := os.Stat(filepath.Join(dir, "demo")); statErr == nil {
		t.Error("JS path should not create a project dir")
	}
}

func TestRun_UnknownLang(t *testing.T) {
	_, err := Run(Options{Lang: "ruby", Dir: t.TempDir()}, &bytes.Buffer{})
	if err == nil {
		t.Fatal("expected error for unknown lang")
	}
	if !strings.Contains(err.Error(), "unknown language") {
		t.Errorf("error should say unknown language: %v", err)
	}
}

func TestRun_RejectsBadName(t *testing.T) {
	_, err := Run(Options{Lang: "python", Name: "Has Spaces", Dir: t.TempDir()}, &bytes.Buffer{})
	if err == nil {
		t.Fatal("expected error for bad name")
	}
	if !strings.Contains(err.Error(), "invalid --name") {
		t.Errorf("error should reject bad name: %v", err)
	}
}

func TestRun_RejectsNonEmptyTarget(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "demo"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "demo", "existing.txt"), []byte("hi"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(Options{Lang: "python", Name: "demo", Dir: dir}, &bytes.Buffer{})
	if err == nil {
		t.Fatal("expected error for non-empty target")
	}
	if !strings.Contains(err.Error(), "not empty") {
		t.Errorf("error should mention non-empty: %v", err)
	}
}

func TestRun_ForceOverridesNonEmpty(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "demo")
	if err := os.MkdirAll(target, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(target, "existing.txt"), []byte("hi"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(Options{Lang: "python", Name: "demo", Dir: dir, Force: true}, &bytes.Buffer{})
	if err != nil {
		t.Fatalf("Run with --force: %v", err)
	}
}

func TestRun_AppPyParsesAsPython(t *testing.T) {
	// Acceptance check: the generated app.py must be syntactically valid Python.
	// We skip if python3 isn't on PATH so the test isn't a hard system dep.
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available on PATH")
	}
	dir := t.TempDir()
	res, err := Run(Options{Lang: "python", Name: "demo", Dir: dir}, &bytes.Buffer{})
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	cmd := exec.Command(python, "-m", "py_compile", filepath.Join(res.TargetDir, "app.py"))
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("py_compile failed: %v\n%s", err, out)
	}
}
