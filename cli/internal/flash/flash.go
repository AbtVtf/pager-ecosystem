// Package flash implements the `pagerctl flash` command.
//
// It detects an attached ESP32-class device by USB VID/PID, then invokes
// esptool to write a firmware binary and rely on esptool's built-in SHA256
// flash-hash check to verify the write.
package flash

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
)

// Options is the parsed CLI input for `pagerctl flash`.
type Options struct {
	Binary string // path to firmware binary (positional argument)
	Port   string // optional override; empty means auto-detect
	Chip   string // default "esp32s3"
	Baud   int    // default 460800
	Offset uint32 // default 0x10000
}

// Defaults applies PagerOS-appropriate defaults to unset fields.
func (o Options) Defaults() Options {
	if o.Chip == "" {
		o.Chip = "esp32s3"
	}
	if o.Baud == 0 {
		o.Baud = 460800
	}
	if o.Offset == 0 {
		// 0x10000 is the standard ESP-IDF application offset and a safe
		// default for single-binary flashes. Use --offset 0x20000 to write
		// to the PagerOS factory (recovery) slot, or 0x0 for a merged image.
		o.Offset = 0x10000
	}
	return o
}

// Run resolves the device, flashes the firmware, and reports the outcome to
// stdout/stderr.
//
// It returns nil on success. On failure it returns a wrapped error and may
// have already written progress/diagnostic output.
func Run(ctx context.Context, opts Options, lister Lister, flasher Flasher, stdout, stderr io.Writer) error {
	opts = opts.Defaults()

	if opts.Binary == "" {
		return errors.New("missing firmware binary argument")
	}
	info, err := os.Stat(opts.Binary)
	if err != nil {
		return fmt.Errorf("firmware binary: %w", err)
	}
	if info.IsDir() {
		return fmt.Errorf("firmware binary: %s is a directory", opts.Binary)
	}

	port, err := SelectPort(lister, opts.Port)
	if err != nil {
		return err
	}

	if port.Match == "user-supplied" {
		fmt.Fprintf(stdout, "Using port %s (user-supplied)\n", port.Name)
	} else {
		fmt.Fprintf(stdout, "Detected %s on %s (VID=%s PID=%s)\n", port.Match, port.Name, port.VID, port.PID)
	}
	fmt.Fprintf(stdout, "Flashing %s (%d bytes) → chip=%s offset=0x%x baud=%d\n",
		opts.Binary, info.Size(), opts.Chip, opts.Offset, opts.Baud)

	req := FlashRequest{
		Port:   port.Name,
		Chip:   opts.Chip,
		Baud:   opts.Baud,
		Offset: opts.Offset,
		Binary: opts.Binary,
	}
	if err := flasher.Flash(ctx, req, stdout, stderr); err != nil {
		return err
	}
	fmt.Fprintln(stdout, "Flash complete; hash verified by esptool.")
	return nil
}
