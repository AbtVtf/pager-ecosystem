package main

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

func TestRunPublish_HelpFlag(t *testing.T) {
	var stderr bytes.Buffer
	err := runPublish(context.Background(), []string{"-h"}, nil, &bytes.Buffer{}, &stderr)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.Contains(stderr.String(), "pagerctl publish") {
		t.Errorf("help output missing header:\n%s", stderr.String())
	}
}

func TestRunPublish_ExtraArgs(t *testing.T) {
	var stderr bytes.Buffer
	err := runPublish(context.Background(), []string{"a.yaml", "b.yaml"}, nil, &bytes.Buffer{}, &stderr)
	if err == nil {
		t.Fatal("expected error for extra args")
	}
	if !strings.Contains(err.Error(), "extra arguments") {
		t.Errorf("error should mention extra arguments: %v", err)
	}
}
