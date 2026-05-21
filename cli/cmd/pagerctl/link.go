package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/pageros/pagerctl/internal/flash"
	"github.com/pageros/pagerctl/internal/link"
)

const linkUsage = `pagerctl link — pair real device to local dev server (CLI-006)

Usage:
  pagerctl link [flags]

Provisions a USB-connected PagerOS device with the Wi-Fi credentials and
dev server URL it needs to fetch Frames from the host machine. After
provisioning the device reboots and reconnects to the dev server; the
live-reload loop served by ` + "`pagerctl dev`" + ` then drives the device's
display end-to-end.

Flags:
  --port <path>      serial port (default: auto-detect ESP32 USB device)
  --baud <n>         console baud rate (default: 115200)
  --ssid <name>      Wi-Fi SSID the device should join (REQUIRED, or use $PAGEROS_WIFI_SSID)
  --pwd <pwd>        Wi-Fi password (default: $PAGEROS_WIFI_PWD; empty = open network)
  --dev-host <host>  dev server host as the device sees it
                     (default: auto-detect host LAN IP; loopback addresses
                     are always rewritten because devices can't reach them)
  --dev-port <n>     dev server port (default: 8000, matches pagerctl dev)
  --url <url>        explicit dev-server URL, bypasses --dev-host/--dev-port
  --no-reboot        provision but skip the post-flight reboot
  -h, --help         show this message

Examples:
  pagerctl link --ssid HomeWifi --pwd hunter2
  pagerctl link --ssid HomeWifi --url http://10.0.0.5:8000/
  PAGEROS_WIFI_SSID=HomeWifi PAGEROS_WIFI_PWD=hunter2 pagerctl link
`

// Env vars accepted as fallbacks for the credential flags. Useful in
// CI-style automation and to avoid putting passwords in shell history.
const (
	envSSID = "PAGEROS_WIFI_SSID"
	envPwd  = "PAGEROS_WIFI_PWD"
)

func runLink(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("link", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, linkUsage) }

	port := fs.String("port", "", "")
	baud := fs.Int("baud", link.DefaultBaud, "")
	ssid := fs.String("ssid", "", "")
	pwd := fs.String("pwd", "", "")
	devHost := fs.String("dev-host", "", "")
	devPort := fs.Int("dev-port", 8000, "")
	url := fs.String("url", "", "")
	noReboot := fs.Bool("no-reboot", false, "")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if pos := fs.Args(); len(pos) > 0 {
		return fmt.Errorf("unexpected extra arguments: %v", pos)
	}

	resolvedSSID := strings.TrimSpace(*ssid)
	if resolvedSSID == "" {
		resolvedSSID = strings.TrimSpace(os.Getenv(envSSID))
	}
	resolvedPwd := *pwd
	if resolvedPwd == "" {
		resolvedPwd = os.Getenv(envPwd)
	}

	opts := link.Options{
		Port:          *port,
		Baud:          *baud,
		SSID:          resolvedSSID,
		Pwd:           resolvedPwd,
		DevServerHost: *devHost,
		DevServerPort: *devPort,
		DevServerURL:  *url,
		SkipReboot:    *noReboot,
	}
	return link.Run(ctx, opts, flash.SystemLister{}, link.SystemOpener{}, link.SystemLAN{}, stdout, stderr)
}
