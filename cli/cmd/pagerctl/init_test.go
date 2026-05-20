package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRunInit_MissingLang(t *testing.T) {
	var stderr bytes.Buffer
	err := runInit(nil, &bytes.Buffer{}, &stderr)
	if err == nil {
		t.Fatal("expected error for missing lang")
	}
}

func TestRunInit_Help(t *testing.T) {
	var stderr bytes.Buffer
	if err := runInit([]string{"-h"}, &bytes.Buffer{}, &stderr); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.Contains(stderr.String(), "pagerctl init") {
		t.Errorf("help output missing header:\n%s", stderr.String())
	}
}

func TestRunInit_PythonEndToEnd(t *testing.T) {
	dir := t.TempDir()
	var stdout, stderr bytes.Buffer
	err := runInit([]string{"python", "--name", "demo", "--dir", dir}, &stdout, &stderr)
	if err != nil {
		t.Fatalf("runInit: %v\nstderr:%s", err, stderr.String())
	}
	if _, err := os.Stat(filepath.Join(dir, "demo", "app.py")); err != nil {
		t.Errorf("expected app.py: %v", err)
	}
	if !strings.Contains(stdout.String(), "Created python app") {
		t.Errorf("stdout missing summary:\n%s", stdout.String())
	}
}
