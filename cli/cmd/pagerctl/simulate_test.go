package main

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

// TestSimulateMissingURL — `pagerctl simulate` with no args must report
// the missing positional and print usage.
func TestSimulateMissingURL(t *testing.T) {
	var stdout, stderr bytes.Buffer
	err := runSimulate(context.Background(), nil, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error for missing url")
	}
	if !strings.Contains(stderr.String(), "Usage:") {
		t.Errorf("stderr should include usage, got %q", stderr.String())
	}
}

// TestSimulateExtraArgs — only one positional URL is accepted.
func TestSimulateExtraArgs(t *testing.T) {
	var stdout, stderr bytes.Buffer
	err := runSimulate(context.Background(), []string{"http://x/", "extra"}, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error for extra args")
	}
	if !strings.Contains(err.Error(), "extra") {
		t.Errorf("error should mention extras, got %q", err.Error())
	}
}

// TestSimulateHelp — `--help` prints usage without erroring.
func TestSimulateHelp(t *testing.T) {
	var stdout, stderr bytes.Buffer
	err := runSimulate(context.Background(), []string{"-h"}, &stdout, &stderr)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.Contains(stderr.String(), "pagerctl simulate") {
		t.Errorf("stderr should include usage, got %q", stderr.String())
	}
}
