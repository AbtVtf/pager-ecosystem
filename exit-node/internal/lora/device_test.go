package lora

import (
	"strings"
	"testing"
)

func TestOpenMissingDeviceReturnsError(t *testing.T) {
	_, err := Open("/dev/null-not-a-real-lora-dev", 115200)
	if err == nil {
		t.Fatal("expected error opening non-existent device")
	}
	if !strings.Contains(err.Error(), "lora:") {
		t.Errorf("error missing lora: prefix: %v", err)
	}
}
