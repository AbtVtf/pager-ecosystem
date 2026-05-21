package link

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"strings"
	"testing"
	"time"
)

func TestPingHappyPath(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":true,"version":"1.2.3","id":"AB12CD34EF56"}`)
	cl := NewClient(mem)

	info, err := cl.Ping(context.Background())
	if err != nil {
		t.Fatalf("ping: %v", err)
	}
	if info.Version != "1.2.3" {
		t.Errorf("version: got %q, want 1.2.3", info.Version)
	}
	if info.ID != "AB12CD34EF56" {
		t.Errorf("id: got %q, want AB12CD34EF56", info.ID)
	}
	got := mem.Written()
	if len(got) != 1 {
		t.Fatalf("expected 1 line written, got %d", len(got))
	}
	var req request
	if err := json.Unmarshal([]byte(got[0]), &req); err != nil {
		t.Fatalf("decode req: %v", err)
	}
	if req.Cmd != "ping" {
		t.Errorf("cmd: got %q, want ping", req.Cmd)
	}
}

func TestSetWiFiOK(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":true}`)
	cl := NewClient(mem)

	if err := cl.SetWiFi(context.Background(), "homenet", "secret123"); err != nil {
		t.Fatalf("set-wifi: %v", err)
	}
	got := mem.Written()
	if len(got) != 1 {
		t.Fatalf("expected 1 line, got %d", len(got))
	}
	var req request
	if err := json.Unmarshal([]byte(got[0]), &req); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if req.Cmd != "set-wifi" {
		t.Errorf("cmd: %q", req.Cmd)
	}
	if req.SSID != "homenet" || req.Pwd != "secret123" {
		t.Errorf("ssid/pwd: got (%q,%q)", req.SSID, req.Pwd)
	}
}

func TestSetWiFiRejectsEmptySSID(t *testing.T) {
	mem := NewMemConn()
	cl := NewClient(mem)
	err := cl.SetWiFi(context.Background(), "", "pw")
	if err == nil || !strings.Contains(err.Error(), "ssid") {
		t.Fatalf("expected ssid error, got %v", err)
	}
	if mem.WrittenLen() != 0 {
		t.Errorf("nothing should have been written, got %d", mem.WrittenLen())
	}
}

func TestSetWiFiAllowsEmptyPassword(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":true}`)
	cl := NewClient(mem)
	if err := cl.SetWiFi(context.Background(), "open-net", ""); err != nil {
		t.Fatalf("open network should be allowed: %v", err)
	}
}

func TestSetDevServerOK(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":true}`)
	cl := NewClient(mem)
	if err := cl.SetDevServer(context.Background(), "http://192.168.1.42:8000/"); err != nil {
		t.Fatalf("set-dev-server: %v", err)
	}
	got := mem.Written()
	var req request
	if err := json.Unmarshal([]byte(got[0]), &req); err != nil {
		t.Fatal(err)
	}
	if req.Cmd != "set-dev-server" || req.URL != "http://192.168.1.42:8000/" {
		t.Errorf("got %+v", req)
	}
}

func TestSetDevServerRejectsEmpty(t *testing.T) {
	mem := NewMemConn()
	cl := NewClient(mem)
	err := cl.SetDevServer(context.Background(), "")
	if err == nil {
		t.Fatal("expected error for empty url")
	}
}

func TestDeviceError(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":false,"error":"nvs full"}`)
	cl := NewClient(mem)
	err := cl.SetWiFi(context.Background(), "net", "pw")
	if err == nil || !strings.Contains(err.Error(), "nvs full") {
		t.Fatalf("expected device error to surface, got %v", err)
	}
}

func TestDeviceErrorWithoutMessage(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":false}`)
	cl := NewClient(mem)
	err := cl.SetWiFi(context.Background(), "net", "pw")
	if err == nil || !strings.Contains(err.Error(), "no detail") {
		t.Fatalf("expected fallback detail, got %v", err)
	}
}

func TestRebootAcceptsSilenceAsSuccess(t *testing.T) {
	mem := NewMemConn()
	cl := NewClient(mem)
	cl.SetTimeout(50 * time.Millisecond)
	// No reply queued — Reboot should treat the timeout as success.
	if err := cl.Reboot(context.Background()); err != nil {
		t.Fatalf("reboot: %v", err)
	}
}

func TestRebootEOFIsSuccess(t *testing.T) {
	mem := NewMemConn()
	cl := NewClient(mem)
	// Closing the conn before Reboot reads triggers an EOF, which
	// should be treated as the device resetting cleanly.
	mem.Close()
	if err := cl.Reboot(context.Background()); err != nil {
		t.Fatalf("reboot on closed conn: %v", err)
	}
}

func TestRebootAcceptsExplicitOK(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":true}`)
	cl := NewClient(mem)
	if err := cl.Reboot(context.Background()); err != nil {
		t.Fatalf("reboot: %v", err)
	}
}

func TestRebootSurfacesDeviceError(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":false,"error":"already pending"}`)
	cl := NewClient(mem)
	err := cl.Reboot(context.Background())
	if err == nil || !strings.Contains(err.Error(), "already pending") {
		t.Fatalf("expected device error, got %v", err)
	}
}

func TestCallTimeout(t *testing.T) {
	mem := NewMemConn()
	cl := NewClient(mem)
	cl.SetTimeout(20 * time.Millisecond)
	_, err := cl.Ping(context.Background())
	if err == nil || !strings.Contains(err.Error(), "no response") {
		t.Fatalf("expected timeout error, got %v", err)
	}
}

func TestParseResponseTolerantToLeadingLog(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`I (1234) console: bla bla {"ok":true,"version":"v1"}`)
	cl := NewClient(mem)
	info, err := cl.Ping(context.Background())
	if err != nil {
		t.Fatalf("ping: %v", err)
	}
	if info.Version != "v1" {
		t.Errorf("expected version v1 even with log prefix, got %q", info.Version)
	}
}

func TestParseResponseRejectsGarbage(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`not even close to json`)
	cl := NewClient(mem)
	_, err := cl.Ping(context.Background())
	if err == nil {
		t.Fatal("expected decode error")
	}
}

func TestParseResponseRejectsEmpty(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(``)
	cl := NewClient(mem)
	_, err := cl.Ping(context.Background())
	if err == nil {
		t.Fatal("expected empty-line error")
	}
}

func TestWriteLineCarriesNewline(t *testing.T) {
	mem := NewMemConn()
	mem.Reply(`{"ok":true}`)
	cl := NewClient(mem)
	_ = cl.SetWiFi(context.Background(), "net", "pw")
	// MemConn.Written strips trailing newlines for ergonomics, but the
	// raw stored bytes must include one so the firmware's line-buffered
	// console sees a complete record.
	mem.mu.Lock()
	defer mem.mu.Unlock()
	if len(mem.written) != 1 || mem.written[0][len(mem.written[0])-1] != '\n' {
		t.Fatalf("expected trailing newline, got %q", mem.written[0])
	}
}

func TestWriteLineMaxLine(t *testing.T) {
	mem := NewMemConn()
	// Write directly through the conn to bypass the JSON wrapper.
	huge := make([]byte, MaxLine+1)
	for i := range huge {
		huge[i] = 'x'
	}
	if err := mem.WriteLine(huge); err != nil {
		t.Fatalf("MemConn does not enforce MaxLine (and is not expected to): %v", err)
	}
}

func TestCallSurfacesUnknownReadError(t *testing.T) {
	mem := NewMemConn()
	cl := NewClient(mem)
	// Close immediately so the read returns io.EOF (not a deadline).
	mem.Close()
	_, err := cl.Ping(context.Background())
	if err == nil || !errors.Is(err, io.EOF) {
		t.Fatalf("expected EOF surface, got %v", err)
	}
}
