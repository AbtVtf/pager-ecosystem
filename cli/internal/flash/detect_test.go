package flash

import (
	"strings"
	"testing"
)

type fakeLister struct {
	ports []Port
	err   error
}

func (f fakeLister) List() ([]Port, error) { return f.ports, f.err }

func TestFilterESP(t *testing.T) {
	all := []Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
		{Name: "/dev/ttyUSB0", VID: "10c4", PID: "ea60", IsUSB: true},
		{Name: "/dev/ttyUSB1", VID: "1a86", PID: "7523", IsUSB: true},
		{Name: "/dev/ttyUSB9", VID: "1234", PID: "abcd", IsUSB: true}, // unrelated
		{Name: "/dev/ttyS0", IsUSB: false},                            // hardware serial — skip
	}
	got := FilterESP(all)
	if len(got) != 3 {
		t.Fatalf("expected 3 ESP candidates, got %d: %+v", len(got), got)
	}
	if got[0].Match == "" || !strings.Contains(got[0].Match, "Espressif") {
		t.Errorf("expected Espressif match on first port, got %q", got[0].Match)
	}
	if got[1].Match == "" || !strings.Contains(got[1].Match, "CP210x") {
		t.Errorf("expected CP210x match on second port, got %q", got[1].Match)
	}
}

func TestFilterESPCaseInsensitive(t *testing.T) {
	all := []Port{{Name: "/dev/ttyACM0", VID: "303A", PID: "1001", IsUSB: true}}
	got := FilterESP(all)
	if len(got) != 1 {
		t.Fatalf("expected upper-case VID to match, got %d", len(got))
	}
}

func TestSelectPortOverride(t *testing.T) {
	l := fakeLister{ports: []Port{{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true}}}
	p, err := SelectPort(l, "/dev/ttyCustom")
	if err != nil {
		t.Fatalf("override should not error: %v", err)
	}
	if p.Name != "/dev/ttyCustom" {
		t.Errorf("expected override port, got %q", p.Name)
	}
	if p.Match != "user-supplied" {
		t.Errorf("expected Match=user-supplied, got %q", p.Match)
	}
}

func TestSelectPortAuto(t *testing.T) {
	l := fakeLister{ports: []Port{
		{Name: "/dev/ttyS0", IsUSB: false},
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
	}}
	p, err := SelectPort(l, "")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if p.Name != "/dev/ttyACM0" {
		t.Errorf("expected /dev/ttyACM0, got %q", p.Name)
	}
}

func TestSelectPortNoneFound(t *testing.T) {
	l := fakeLister{ports: []Port{{Name: "/dev/ttyS0", IsUSB: false}}}
	_, err := SelectPort(l, "")
	if err == nil || !strings.Contains(err.Error(), "no ESP32 device detected") {
		t.Fatalf("expected no-device error, got %v", err)
	}
}

func TestSelectPortAmbiguous(t *testing.T) {
	l := fakeLister{ports: []Port{
		{Name: "/dev/ttyACM0", VID: "303a", PID: "1001", IsUSB: true},
		{Name: "/dev/ttyUSB0", VID: "10c4", PID: "ea60", IsUSB: true},
	}}
	_, err := SelectPort(l, "")
	if err == nil || !strings.Contains(err.Error(), "multiple ESP32 devices detected") {
		t.Fatalf("expected ambiguity error, got %v", err)
	}
}

func TestSelectPortListError(t *testing.T) {
	l := fakeLister{err: errStub("boom")}
	_, err := SelectPort(l, "")
	if err == nil {
		t.Fatal("expected propagated list error")
	}
}

type errStub string

func (e errStub) Error() string { return string(e) }
