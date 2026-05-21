// Package link implements `pagerctl link` — pairing a real device to a
// local dev server over USB-CDC serial.
//
// # Wire protocol
//
// The firmware exposes a line-based JSON console on its USB-CDC serial
// port at 115200 baud, 8N1. The CLI writes one JSON object per line and
// reads one JSON object per line in response. The transport is
// human-pasteable so a developer with a plain terminal can speak the
// same protocol by hand for debugging.
//
//	→  {"cmd":"ping"}
//	←  {"ok":true,"version":"...","id":"<device-fingerprint>"}
//
//	→  {"cmd":"set-wifi","ssid":"...","pwd":"..."}
//	←  {"ok":true}
//
//	→  {"cmd":"set-dev-server","url":"http://192.168.1.42:8000/"}
//	←  {"ok":true}
//
//	→  {"cmd":"reboot"}
//	←  {"ok":true}            (sent before reset; absence is not an error)
//
// On any failure the response is `{"ok":false,"error":"<message>"}` and
// the CLI surfaces the message verbatim to the user.
//
// The protocol is intentionally minimal so the firmware side (an
// esp_console REPL command) is a few hundred lines of glue around
// existing NVS + pageros_wifi APIs. See the README in this directory
// for the firmware contract.
package link

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strings"
	"time"
)

// DefaultBaud is the USB-CDC console baud rate. ESP32-S3's native
// USB-Serial-JTAG port ignores baud and always runs at native USB speed,
// but the field is honoured by external USB-UART bridges (CP210x,
// CH340), so we set a sensible default.
const DefaultBaud = 115200

// DefaultTimeout caps how long the CLI waits for any single response
// line. Firmware command handlers are simple key-value writes to NVS,
// well below 500 ms; 5 s gives generous slack for slow USB stacks
// without making a hung device feel unresponsive.
const DefaultTimeout = 5 * time.Second

// RebootGrace is how long we wait after `reboot` before declaring
// success. The device may not respond before resetting, so the absence
// of a reply within this window is treated as the expected outcome.
const RebootGrace = 1500 * time.Millisecond

// MaxLine bounds the size of any single JSON line in either direction.
// 4 KiB is comfortable for a Wi-Fi SSID + password + dev URL with
// headroom; anything larger almost certainly indicates a desync (e.g.
// a log line bleeding into the response channel).
const MaxLine = 4 * 1024

// PingInfo is the structured body of a successful `ping` response. The
// CLI surfaces these fields to the user so they can confirm they're
// talking to the right device.
type PingInfo struct {
	Version string `json:"version,omitempty"`
	ID      string `json:"id,omitempty"`
}

// rawResponse is the on-wire shape of every reply. Additional fields
// (`version`, `id`, ...) are decoded out of band into a typed struct.
type rawResponse struct {
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
}

// request is the on-wire shape of every command. We keep field names
// stable across versions; new commands extend the protocol additively.
type request struct {
	Cmd  string `json:"cmd"`
	SSID string `json:"ssid,omitempty"`
	Pwd  string `json:"pwd,omitempty"`
	URL  string `json:"url,omitempty"`
}

// Conn is a duplex line-delimited transport. The link package owns
// neither the reconnect logic nor the read timeout — both live in the
// SerialConn implementation in transport.go. Tests substitute a
// channel-backed fake.
type Conn interface {
	// Write sends a single JSON line. The implementation appends "\n".
	WriteLine(line []byte) error
	// ReadLine blocks until a newline-terminated line is available or
	// ctx is cancelled. Returned bytes exclude the trailing "\n".
	ReadLine(ctx context.Context) ([]byte, error)
	io.Closer
}

// Client speaks the line-based JSON protocol over a Conn. It is the
// stateless half of the package — orchestration (which commands to
// send, in what order) lives in Run.
type Client struct {
	c       Conn
	timeout time.Duration
}

// NewClient wraps conn with default per-command timeouts.
func NewClient(conn Conn) *Client {
	return &Client{c: conn, timeout: DefaultTimeout}
}

// SetTimeout overrides DefaultTimeout. Mostly useful in tests; the
// production path leaves it at the default.
func (cl *Client) SetTimeout(d time.Duration) {
	if d > 0 {
		cl.timeout = d
	}
}

// Ping issues a ping and decodes the optional version/id metadata.
func (cl *Client) Ping(ctx context.Context) (PingInfo, error) {
	body, err := cl.call(ctx, request{Cmd: "ping"})
	if err != nil {
		return PingInfo{}, err
	}
	var info PingInfo
	if len(body) > 0 {
		if err := json.Unmarshal(body, &info); err != nil {
			return PingInfo{}, fmt.Errorf("ping: decode response: %w", err)
		}
	}
	return info, nil
}

// SetWiFi provisions station credentials. An empty password is accepted
// for open networks; the firmware persists what it receives unchanged.
func (cl *Client) SetWiFi(ctx context.Context, ssid, pwd string) error {
	if strings.TrimSpace(ssid) == "" {
		return errors.New("ssid must not be empty")
	}
	_, err := cl.call(ctx, request{Cmd: "set-wifi", SSID: ssid, Pwd: pwd})
	return err
}

// SetDevServer points the device's frame fetcher at url. The device is
// expected to validate the URL shape and reject obviously malformed
// inputs; the CLI does its own URL validation in ResolveDevServerURL.
func (cl *Client) SetDevServer(ctx context.Context, url string) error {
	if strings.TrimSpace(url) == "" {
		return errors.New("url must not be empty")
	}
	_, err := cl.call(ctx, request{Cmd: "set-dev-server", URL: url})
	return err
}

// Reboot requests an immediate reset. The device may reset before its
// `{"ok":true}` reaches the host, so a read timeout is treated as
// success after RebootGrace has elapsed.
func (cl *Client) Reboot(ctx context.Context) error {
	line, err := json.Marshal(request{Cmd: "reboot"})
	if err != nil {
		return fmt.Errorf("reboot: encode: %w", err)
	}
	if err := cl.c.WriteLine(line); err != nil {
		return fmt.Errorf("reboot: write: %w", err)
	}
	readCtx, cancel := context.WithTimeout(ctx, RebootGrace)
	defer cancel()
	raw, err := cl.c.ReadLine(readCtx)
	if err != nil {
		// Device reset before replying — that is the expected outcome.
		if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, io.EOF) {
			return nil
		}
		return fmt.Errorf("reboot: %w", err)
	}
	resp, _, err := parseResponse(raw)
	if err != nil {
		return fmt.Errorf("reboot: %w", err)
	}
	if !resp.OK {
		return fmt.Errorf("reboot: device error: %s", responseErrMsg(resp))
	}
	return nil
}

func (cl *Client) call(ctx context.Context, req request) (json.RawMessage, error) {
	line, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("%s: encode: %w", req.Cmd, err)
	}
	if err := cl.c.WriteLine(line); err != nil {
		return nil, fmt.Errorf("%s: write: %w", req.Cmd, err)
	}
	readCtx, cancel := context.WithTimeout(ctx, cl.timeout)
	defer cancel()
	raw, err := cl.c.ReadLine(readCtx)
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) {
			return nil, fmt.Errorf("%s: no response within %s (is the firmware build linked against the provisioning console?)", req.Cmd, cl.timeout)
		}
		return nil, fmt.Errorf("%s: read: %w", req.Cmd, err)
	}
	resp, rest, err := parseResponse(raw)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", req.Cmd, err)
	}
	if !resp.OK {
		return nil, fmt.Errorf("%s: device error: %s", req.Cmd, responseErrMsg(resp))
	}
	return rest, nil
}

// parseResponse decodes the {ok,error} envelope and returns the raw
// JSON object so callers can pull command-specific fields out of it.
func parseResponse(raw []byte) (rawResponse, json.RawMessage, error) {
	raw = trimNoise(raw)
	if len(raw) == 0 {
		return rawResponse{}, nil, errors.New("empty response line")
	}
	var resp rawResponse
	if err := json.Unmarshal(raw, &resp); err != nil {
		return rawResponse{}, nil, fmt.Errorf("decode response %q: %w", truncate(raw, 120), err)
	}
	return resp, raw, nil
}

func responseErrMsg(r rawResponse) string {
	if r.Error == "" {
		return "(no detail)"
	}
	return r.Error
}

// trimNoise discards leading log clutter from the firmware's serial
// output. ESP-IDF prints ANSI-coloured log lines on the same UART as
// the console; we tolerate that by skipping any prefix that doesn't
// look like a JSON object. (A well-behaved firmware build will route
// logs to a separate UART, but we don't depend on it.)
func trimNoise(raw []byte) []byte {
	for i, b := range raw {
		if b == '{' {
			return raw[i:]
		}
	}
	return nil
}

func truncate(b []byte, n int) string {
	if len(b) <= n {
		return string(b)
	}
	return string(b[:n]) + "..."
}

// scanLines is a small helper used by the SerialConn implementation to
// read newline-delimited records with a bounded buffer.
func scanLines(r io.Reader) *bufio.Scanner {
	s := bufio.NewScanner(r)
	buf := make([]byte, 0, MaxLine)
	s.Buffer(buf, MaxLine)
	s.Split(bufio.ScanLines)
	return s
}
