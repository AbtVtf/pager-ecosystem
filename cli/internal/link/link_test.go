package link

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/pageros/pagerctl/internal/flash"
)

type fakeFlashLister struct {
	ports []flash.Port
	err   error
}

func (f fakeFlashLister) List() ([]flash.Port, error) { return f.ports, f.err }

// loadHappyPathReplies queues a ping + set-wifi + set-dev-server +
// reboot OK reply sequence onto the supplied MemConn.
func loadHappyPathReplies(m *MemConn) {
	m.Reply(`{"ok":true,"id":"AB12CD34EF56","version":"0.1.0"}`)
	m.Reply(`{"ok":true}`)
	m.Reply(`{"ok":true}`)
	m.Reply(`{"ok":true}`)
}

func TestRunHappyPath(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	conn := NewMemConn()
	loadHappyPathReplies(conn)
	opener := &MemOpener{Conn: conn}
	lan := fakeLAN{ip: "192.168.1.42"}

	var stdout, stderr bytes.Buffer
	opts := Options{
		SSID:          "homenet",
		Pwd:           "secret",
		DevServerPort: 8000,
	}
	if err := Run(context.Background(), opts, lister, opener, lan, &stdout, &stderr); err != nil {
		t.Fatalf("Run: %v", err)
	}

	// The auto-detected port should have been opened at the default baud.
	if opener.Port != "/dev/ttyACM0" {
		t.Errorf("port opened: %q", opener.Port)
	}
	if opener.Baud != DefaultBaud {
		t.Errorf("baud: got %d, want %d", opener.Baud, DefaultBaud)
	}

	written := conn.Written()
	if len(written) != 4 {
		t.Fatalf("expected 4 commands sent, got %d: %v", len(written), written)
	}
	cmds := commandsOf(t, written)
	want := []string{"ping", "set-wifi", "set-dev-server", "reboot"}
	for i := range want {
		if cmds[i] != want[i] {
			t.Errorf("cmd %d: got %q, want %q", i, cmds[i], want[i])
		}
	}

	// The set-dev-server URL must use the LAN address, not loopback.
	var devReq request
	if err := json.Unmarshal([]byte(written[2]), &devReq); err != nil {
		t.Fatal(err)
	}
	if devReq.URL != "http://192.168.1.42:8000/" {
		t.Errorf("dev-server url: got %q", devReq.URL)
	}

	out := stdout.String()
	if !strings.Contains(out, "Detected Espressif USB-Serial-JTAG") {
		t.Errorf("expected device detection line, got: %q", out)
	}
	if !strings.Contains(out, "device AB12CD34EF56 (firmware 0.1.0)") {
		t.Errorf("expected ping info line, got: %q", out)
	}
	if !strings.Contains(out, "Linked.") {
		t.Errorf("expected success line, got: %q", out)
	}
	if strings.Contains(out, "secret") {
		t.Errorf("password leaked to stdout: %q", out)
	}
	if !strings.Contains(out, "***6-char***") {
		t.Errorf("expected censored password marker, got: %q", out)
	}
}

func TestRunRequiresSSID(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	opener := &MemOpener{Conn: NewMemConn()}

	var stdout, stderr bytes.Buffer
	opts := Options{
		Pwd:           "pw",
		DevServerPort: 8000,
	}
	err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "1.2.3.4"}, &stdout, &stderr)
	if err == nil || !strings.Contains(err.Error(), "--ssid is required") {
		t.Fatalf("expected ssid error, got %v", err)
	}
	// We should fail before opening the port.
	if opener.Port != "" {
		t.Errorf("port should not have been opened, got %q", opener.Port)
	}
}

func TestRunHonoursDevServerURLOverride(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	conn := NewMemConn()
	loadHappyPathReplies(conn)
	opener := &MemOpener{Conn: conn}

	// LAN resolver should not be consulted when the user supplies the URL.
	lan := fakeLAN{err: errors.New("must not be called")}
	opts := Options{
		SSID:         "homenet",
		Pwd:          "secret",
		DevServerURL: "http://tunnel.example.com/",
	}
	var stdout, stderr bytes.Buffer
	if err := Run(context.Background(), opts, lister, opener, lan, &stdout, &stderr); err != nil {
		t.Fatalf("Run: %v", err)
	}
	written := conn.Written()
	var devReq request
	if err := json.Unmarshal([]byte(written[2]), &devReq); err != nil {
		t.Fatal(err)
	}
	if devReq.URL != "http://tunnel.example.com/" {
		t.Errorf("url override ignored, got %q", devReq.URL)
	}
}

func TestRunSkipReboot(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	conn := NewMemConn()
	// ping + set-wifi + set-dev-server (no reboot)
	conn.Reply(`{"ok":true}`)
	conn.Reply(`{"ok":true}`)
	conn.Reply(`{"ok":true}`)
	opener := &MemOpener{Conn: conn}

	var stdout, stderr bytes.Buffer
	opts := Options{
		SSID:       "homenet",
		Pwd:        "pw",
		SkipReboot: true,
	}
	if err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "10.0.0.1"}, &stdout, &stderr); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if got := conn.WrittenLen(); got != 3 {
		t.Errorf("expected 3 commands when --no-reboot, got %d", got)
	}
	if !strings.Contains(stdout.String(), "Skipping reboot") {
		t.Errorf("expected skip-reboot notice, got %q", stdout.String())
	}
}

func TestRunPropagatesDeviceError(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	conn := NewMemConn()
	conn.Reply(`{"ok":true}`)
	conn.Reply(`{"ok":false,"error":"nvs corrupt"}`)
	opener := &MemOpener{Conn: conn}

	var stdout, stderr bytes.Buffer
	opts := Options{SSID: "net", Pwd: "pw"}
	err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "10.0.0.1"}, &stdout, &stderr)
	if err == nil || !strings.Contains(err.Error(), "nvs corrupt") {
		t.Fatalf("expected nvs error to surface, got %v", err)
	}
}

func TestRunNoDevice(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyS0", IsUSB: false},
	}}
	opener := &MemOpener{Conn: NewMemConn()}
	var stdout, stderr bytes.Buffer
	opts := Options{SSID: "net"}
	err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "1.2.3.4"}, &stdout, &stderr)
	if err == nil || !strings.Contains(err.Error(), "no ESP32 device") {
		t.Fatalf("expected no-device error, got %v", err)
	}
}

func TestRunOpenError(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	opener := &MemOpener{OpenErr: errors.New("device busy")}
	var stdout, stderr bytes.Buffer
	opts := Options{SSID: "net"}
	err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "1.2.3.4"}, &stdout, &stderr)
	if err == nil || !strings.Contains(err.Error(), "device busy") {
		t.Fatalf("expected open error, got %v", err)
	}
}

func TestRunUserSuppliedPort(t *testing.T) {
	lister := fakeFlashLister{} // empty — auto-detect would fail
	conn := NewMemConn()
	loadHappyPathReplies(conn)
	opener := &MemOpener{Conn: conn}

	var stdout, stderr bytes.Buffer
	opts := Options{
		SSID: "net",
		Pwd:  "pw",
		Port: "/dev/ttyCustom",
	}
	if err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "1.2.3.4"}, &stdout, &stderr); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if opener.Port != "/dev/ttyCustom" {
		t.Errorf("expected user-supplied port, got %q", opener.Port)
	}
	if !strings.Contains(stdout.String(), "user-supplied") {
		t.Errorf("expected user-supplied notice, got: %q", stdout.String())
	}
}

func TestRunCustomBaud(t *testing.T) {
	lister := fakeFlashLister{ports: []flash.Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	conn := NewMemConn()
	loadHappyPathReplies(conn)
	opener := &MemOpener{Conn: conn}

	var stdout, stderr bytes.Buffer
	opts := Options{SSID: "net", Pwd: "pw", Baud: 921600}
	if err := Run(context.Background(), opts, lister, opener, fakeLAN{ip: "1.2.3.4"}, &stdout, &stderr); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if opener.Baud != 921600 {
		t.Errorf("baud: got %d, want 921600", opener.Baud)
	}
}

func TestDefaultsRejectBadDevPort(t *testing.T) {
	_, err := Options{SSID: "x", DevServerPort: -1}.Defaults()
	if err == nil {
		t.Fatal("expected port validation")
	}
}

func TestDefaultsAllowEmptyPassword(t *testing.T) {
	opts, err := Options{SSID: "open"}.Defaults()
	if err != nil {
		t.Fatalf("defaults: %v", err)
	}
	if opts.Pwd != "" {
		t.Errorf("password should remain empty for open networks, got %q", opts.Pwd)
	}
}

// commandsOf decodes the "cmd" field of every written line, asserting
// each is valid JSON so a typo in the on-wire format surfaces here.
func commandsOf(t *testing.T, lines []string) []string {
	t.Helper()
	out := make([]string, len(lines))
	for i, l := range lines {
		var r request
		if err := json.Unmarshal([]byte(l), &r); err != nil {
			t.Fatalf("line %d not JSON: %q (%v)", i, l, err)
		}
		out[i] = r.Cmd
	}
	return out
}
