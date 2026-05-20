package flash

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type fakeFlasher struct {
	called bool
	gotReq FlashRequest
	err    error
}

func (f *fakeFlasher) Flash(ctx context.Context, req FlashRequest, stdout, stderr io.Writer) error {
	f.called = true
	f.gotReq = req
	if f.err != nil {
		return f.err
	}
	io.WriteString(stdout, "(esptool output)\n")
	return nil
}

func writeTempBin(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	p := filepath.Join(dir, "fw.bin")
	if err := os.WriteFile(p, []byte("not a real firmware but length>0"), 0o644); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	return p
}

func TestRunHappyPath(t *testing.T) {
	bin := writeTempBin(t)
	l := fakeLister{ports: []Port{{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true}}}
	f := &fakeFlasher{}
	var stdout, stderr bytes.Buffer

	err := Run(context.Background(), Options{Binary: bin}, l, f, &stdout, &stderr)
	if err != nil {
		t.Fatalf("Run failed: %v", err)
	}
	if !f.called {
		t.Fatal("flasher was not invoked")
	}
	if f.gotReq.Port != "/dev/ttyACM0" {
		t.Errorf("expected detected port, got %q", f.gotReq.Port)
	}
	if f.gotReq.Chip != "esp32s3" {
		t.Errorf("expected default chip esp32s3, got %q", f.gotReq.Chip)
	}
	if f.gotReq.Baud != 460800 {
		t.Errorf("expected default baud 460800, got %d", f.gotReq.Baud)
	}
	if f.gotReq.Offset != 0x10000 {
		t.Errorf("expected default offset 0x10000, got 0x%x", f.gotReq.Offset)
	}
	out := stdout.String()
	if !strings.Contains(out, "Detected Espressif USB-Serial-JTAG") {
		t.Errorf("expected detection message, got:\n%s", out)
	}
	if !strings.Contains(out, "Flash complete") {
		t.Errorf("expected success message, got:\n%s", out)
	}
}

func TestRunRespectsOverrides(t *testing.T) {
	bin := writeTempBin(t)
	l := fakeLister{} // should not be consulted
	f := &fakeFlasher{}
	opts := Options{
		Binary: bin,
		Port:   "/dev/ttyXYZ",
		Chip:   "esp32",
		Baud:   921600,
		Offset: 0x20000,
	}
	err := Run(context.Background(), opts, l, f, io.Discard, io.Discard)
	if err != nil {
		t.Fatalf("Run failed: %v", err)
	}
	if f.gotReq.Port != "/dev/ttyXYZ" {
		t.Errorf("expected /dev/ttyXYZ, got %q", f.gotReq.Port)
	}
	if f.gotReq.Chip != "esp32" {
		t.Errorf("expected esp32, got %q", f.gotReq.Chip)
	}
	if f.gotReq.Baud != 921600 {
		t.Errorf("expected 921600, got %d", f.gotReq.Baud)
	}
	if f.gotReq.Offset != 0x20000 {
		t.Errorf("expected 0x20000, got 0x%x", f.gotReq.Offset)
	}
}

func TestRunMissingBinary(t *testing.T) {
	l := fakeLister{}
	f := &fakeFlasher{}
	err := Run(context.Background(), Options{}, l, f, io.Discard, io.Discard)
	if err == nil || !strings.Contains(err.Error(), "missing firmware binary") {
		t.Fatalf("expected missing-binary error, got %v", err)
	}
	if f.called {
		t.Error("flasher should not be called without a binary")
	}
}

func TestRunBinaryNotFound(t *testing.T) {
	l := fakeLister{ports: []Port{{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true}}}
	f := &fakeFlasher{}
	err := Run(context.Background(), Options{Binary: "/nonexistent/fw.bin"}, l, f, io.Discard, io.Discard)
	if err == nil {
		t.Fatal("expected error for missing binary")
	}
	if f.called {
		t.Error("flasher should not be called when binary is missing")
	}
}

func TestRunPropagatesFlasherError(t *testing.T) {
	bin := writeTempBin(t)
	l := fakeLister{ports: []Port{{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true}}}
	f := &fakeFlasher{err: errors.New("write failed")}
	err := Run(context.Background(), Options{Binary: bin}, l, f, io.Discard, io.Discard)
	if err == nil || !strings.Contains(err.Error(), "write failed") {
		t.Fatalf("expected propagated error, got %v", err)
	}
}

func TestFlashRequestValidation(t *testing.T) {
	cases := []struct {
		name string
		req  FlashRequest
	}{
		{"no port", FlashRequest{Chip: "esp32s3", Binary: "f", Baud: 1}},
		{"no chip", FlashRequest{Port: "p", Binary: "f", Baud: 1}},
		{"no binary", FlashRequest{Port: "p", Chip: "esp32s3", Baud: 1}},
		{"zero baud", FlashRequest{Port: "p", Chip: "esp32s3", Binary: "f"}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			err := Esptool{Bin: "/usr/bin/true"}.Flash(context.Background(), c.req, io.Discard, io.Discard)
			if err == nil {
				t.Fatal("expected validation error")
			}
		})
	}
}
