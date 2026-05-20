package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"strconv"
	"strings"

	"github.com/pageros/pagerctl/internal/flash"
)

const flashUsage = `pagerctl flash — flash firmware to attached device

Usage:
  pagerctl flash [flags] <fw.bin>

Flags:
  --port <path>     serial port (default: auto-detect ESP32 USB device)
  --chip <name>     target chip (default: esp32s3)
  --baud <n>        baud rate (default: 460800)
  --offset <hex>    flash offset in hex, e.g. 0x10000 (default: 0x10000)
                    Use 0x20000 to write to the PagerOS factory recovery slot.
  -h, --help        show this message
`

func runFlash(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("flash", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, flashUsage) }

	port := fs.String("port", "", "")
	chip := fs.String("chip", "esp32s3", "")
	baud := fs.Int("baud", 460800, "")
	offset := fs.String("offset", "0x10000", "")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}

	pos := fs.Args()
	if len(pos) == 0 {
		fmt.Fprint(stderr, flashUsage)
		return fmt.Errorf("missing <fw.bin> argument")
	}
	if len(pos) > 1 {
		return fmt.Errorf("unexpected extra arguments: %v", pos[1:])
	}

	off, err := parseOffset(*offset)
	if err != nil {
		return err
	}

	opts := flash.Options{
		Binary: pos[0],
		Port:   *port,
		Chip:   *chip,
		Baud:   *baud,
		Offset: off,
	}
	return flash.Run(ctx, opts, flash.SystemLister{}, flash.Esptool{}, stdout, stderr)
}

func parseOffset(s string) (uint32, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, fmt.Errorf("empty offset")
	}
	// Accept 0x-prefixed hex, plain hex, or decimal.
	base := 10
	if strings.HasPrefix(strings.ToLower(s), "0x") {
		s = s[2:]
		base = 16
	} else if strings.ContainsAny(s, "abcdefABCDEF") {
		base = 16
	}
	v, err := strconv.ParseUint(s, base, 32)
	if err != nil {
		return 0, fmt.Errorf("invalid --offset %q: %w", s, err)
	}
	return uint32(v), nil
}
