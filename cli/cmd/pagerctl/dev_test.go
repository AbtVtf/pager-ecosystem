package main

import (
	"bytes"
	"context"
	"os"
	"strings"
	"testing"
)

// TestDevHelp — `--help` prints usage without erroring.
func TestDevHelp(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if err := runDev(context.Background(), []string{"-h"}, &stdout, &stderr); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.Contains(stderr.String(), "pagerctl dev") {
		t.Errorf("usage missing, got %q", stderr.String())
	}
}

// TestDevExtraArgs — `dev` takes no positionals.
func TestDevExtraArgs(t *testing.T) {
	var stdout, stderr bytes.Buffer
	err := runDev(context.Background(), []string{"unexpected"}, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error for extra positional")
	}
	if !strings.Contains(err.Error(), "unexpected") {
		t.Errorf("error should mention extras, got %q", err.Error())
	}
}

// TestDevMissingApp — default `app.py` is reported when it does not
// exist in the cwd. We chdir to a known-empty tempdir so this doesn't
// pick up an unrelated app.py.
func TestDevMissingApp(t *testing.T) {
	orig, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chdir(t.TempDir()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(orig) })

	var stdout, stderr bytes.Buffer
	err = runDev(context.Background(), nil, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error when app.py is missing")
	}
	if !strings.Contains(err.Error(), "app.py") {
		t.Errorf("error should mention app.py, got %q", err.Error())
	}
}
