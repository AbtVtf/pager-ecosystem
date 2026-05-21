package link

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strings"

	"github.com/pageros/pagerctl/internal/flash"
)

// Options is the parsed CLI input for `pagerctl link`.
type Options struct {
	// Port is an optional serial-port override. Empty means use the same
	// auto-detect logic as `pagerctl flash`: pick the single ESP32 USB
	// device on the bus, error if zero or many.
	Port string
	// Baud overrides DefaultBaud. Mostly useful for non-S3 silicon over
	// an external USB-UART bridge.
	Baud int
	// SSID + Pwd are the Wi-Fi credentials the device should join.
	// Pwd may be empty for open networks; SSID must be non-empty.
	SSID string
	Pwd  string
	// DevServerHost / DevServerPort describe where the dev server is
	// listening on the host machine. If host is empty or loopback it is
	// rewritten to a routable LAN address. Defaults mirror the dev
	// command (127.0.0.1:8000) so `pagerctl dev` + `pagerctl link` Just
	// Works without extra flags.
	DevServerHost string
	DevServerPort int
	// DevServerURL is an explicit override that bypasses host/port
	// resolution. Pass when running the dev server behind a tunnel or
	// reverse proxy where the device should connect to some other URL.
	DevServerURL string
	// SkipReboot leaves the device running. Used when the developer
	// wants to call `pagerctl link` repeatedly (e.g. rotating Wi-Fi
	// credentials during testing) without dropping the active session.
	SkipReboot bool
}

// Defaults fills in unset fields. Returns an error only if a supplied
// value is malformed.
func (o Options) Defaults() (Options, error) {
	if o.Baud == 0 {
		o.Baud = DefaultBaud
	}
	if o.DevServerPort == 0 {
		o.DevServerPort = 8000
	}
	if o.DevServerHost == "" && o.DevServerURL == "" {
		o.DevServerHost = DefaultLoopback
	}
	if strings.TrimSpace(o.SSID) == "" {
		return o, errors.New("--ssid is required")
	}
	if o.DevServerPort < 1 || o.DevServerPort > 65535 {
		return o, fmt.Errorf("invalid --port-dev %d (must be 1-65535)", o.DevServerPort)
	}
	return o, nil
}

// Run executes the link workflow: detect device → open serial → ping →
// set wifi → set dev server → reboot. Each step writes a short progress
// line to stdout so a developer watching the terminal sees what failed
// when the chain breaks.
func Run(
	ctx context.Context,
	opts Options,
	lister flash.Lister,
	opener Opener,
	resolver LANResolver,
	stdout, stderr io.Writer,
) error {
	opts, err := opts.Defaults()
	if err != nil {
		return err
	}

	url := opts.DevServerURL
	if url == "" {
		url, err = ResolveDevServerURL(opts.DevServerHost, opts.DevServerPort, resolver)
		if err != nil {
			return err
		}
	}

	port, err := flash.SelectPort(lister, opts.Port)
	if err != nil {
		return err
	}
	if port.Match == "user-supplied" {
		fmt.Fprintf(stdout, "Using port %s (user-supplied)\n", port.Name)
	} else {
		fmt.Fprintf(stdout, "Detected %s on %s (VID=%s PID=%s)\n", port.Match, port.Name, port.VID, port.PID)
	}

	conn, err := opener.Open(port.Name, opts.Baud)
	if err != nil {
		return err
	}
	defer conn.Close()

	client := NewClient(conn)

	fmt.Fprintf(stdout, "Pinging device...\n")
	info, err := client.Ping(ctx)
	if err != nil {
		return err
	}
	switch {
	case info.ID != "" && info.Version != "":
		fmt.Fprintf(stdout, "  device %s (firmware %s)\n", info.ID, info.Version)
	case info.ID != "":
		fmt.Fprintf(stdout, "  device %s\n", info.ID)
	}

	fmt.Fprintf(stdout, "Setting Wi-Fi SSID=%q (password %s)\n", opts.SSID, censor(opts.Pwd))
	if err := client.SetWiFi(ctx, opts.SSID, opts.Pwd); err != nil {
		return err
	}

	fmt.Fprintf(stdout, "Setting dev server URL %s\n", url)
	if err := client.SetDevServer(ctx, url); err != nil {
		return err
	}

	if opts.SkipReboot {
		fmt.Fprintf(stdout, "Skipping reboot (--no-reboot). Device will pick up new settings on next restart.\n")
		return nil
	}

	fmt.Fprintf(stdout, "Rebooting device...\n")
	if err := client.Reboot(ctx); err != nil {
		return err
	}
	fmt.Fprintf(stdout, "Linked. Device should reconnect to %q and start pulling Frames from %s.\n", opts.SSID, url)
	return nil
}

// censor renders a password as ***N*** where N is the original length,
// so logs show whether the user typed something without leaking it.
// Returns "(empty)" for the open-network case so the operator sees the
// intent rather than nothing at all.
func censor(s string) string {
	if s == "" {
		return "(empty)"
	}
	return fmt.Sprintf("***%d-char***", len(s))
}
