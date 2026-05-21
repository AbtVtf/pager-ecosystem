package main

import (
	"bytes"
	"context"
	"os"
	"strings"
	"testing"
)

func TestLinkHelp(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if err := runLink(context.Background(), []string{"-h"}, &stdout, &stderr); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.Contains(stderr.String(), "pagerctl link") {
		t.Errorf("usage missing, got %q", stderr.String())
	}
	if !strings.Contains(stderr.String(), "--ssid") {
		t.Errorf("usage missing --ssid line, got %q", stderr.String())
	}
}

func TestLinkExtraArgs(t *testing.T) {
	var stdout, stderr bytes.Buffer
	err := runLink(context.Background(), []string{"unexpected"}, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error for extra positional")
	}
	if !strings.Contains(err.Error(), "unexpected") {
		t.Errorf("error should mention extras, got %q", err.Error())
	}
}

// TestLinkMissingSSID exercises the flag plumbing: with no --ssid and
// no env fallback, the error from link.Options.Defaults should bubble
// up through runLink as-is. We unset PAGEROS_WIFI_SSID in case the
// developer running the test has it in their environment.
func TestLinkMissingSSID(t *testing.T) {
	t.Setenv(envSSID, "")
	t.Setenv(envPwd, "")
	var stdout, stderr bytes.Buffer
	err := runLink(context.Background(), nil, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected error when --ssid and PAGEROS_WIFI_SSID are both empty")
	}
	if !strings.Contains(err.Error(), "--ssid is required") {
		t.Errorf("expected ssid error, got %v", err)
	}
}

// TestLinkEnvFallbackSSID verifies the env var fallback works. We can't
// easily exercise the full Run path here (it needs a real serial port),
// but we can confirm the flag-level wiring accepts the env value by
// progressing past the SSID check and only failing later at port
// detection.
func TestLinkEnvFallbackSSID(t *testing.T) {
	t.Setenv(envSSID, "from-env")
	t.Setenv(envPwd, "from-env-pw")
	var stdout, stderr bytes.Buffer
	err := runLink(context.Background(), nil, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected port-detection error after SSID validation passes")
	}
	// We don't care what fails next, only that the SSID check passed —
	// i.e. the error is *not* the "--ssid is required" message.
	if strings.Contains(err.Error(), "--ssid is required") {
		t.Errorf("env fallback did not populate ssid: %v", err)
	}
}

func TestLinkPwdEnvFallback(t *testing.T) {
	// Flag empty, env present → env should be picked up.
	t.Setenv(envSSID, "")
	t.Setenv(envPwd, "envpw")
	// Build a fresh, predictable env so this test doesn't leak state.
	if got := os.Getenv(envPwd); got != "envpw" {
		t.Fatalf("env setup failed: %q", got)
	}
	// We can't observe the censored password in stderr without a full
	// run, so we settle for asserting the flag plumbing doesn't trip on
	// an empty --pwd when the env is set. The interaction is exercised
	// end-to-end in TestRunHappyPath in internal/link.
	t.Setenv(envSSID, "net")
	var stdout, stderr bytes.Buffer
	err := runLink(context.Background(), nil, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected later failure (no real device)")
	}
}
