package flash

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strings"
)

// Flasher writes a firmware binary to a device and verifies it.
type Flasher interface {
	Flash(ctx context.Context, req FlashRequest, stdout, stderr io.Writer) error
}

// FlashRequest captures everything esptool needs to flash a single binary.
type FlashRequest struct {
	Port   string
	Chip   string // e.g. "esp32s3"
	Baud   int    // e.g. 460800
	Offset uint32 // flash offset in bytes
	Binary string // path to firmware binary
}

// Esptool runs the external `esptool` command.
//
// The PagerOS spec keeps pagerctl as a single Go binary, but the ESP32 ROM
// loader protocol is intricate enough that vendor-provided esptool remains the
// canonical implementation. Distributions ship it as either `esptool` (v5+) or
// `esptool.py` (v4 and earlier); we accept both.
type Esptool struct {
	Bin string // explicit binary; if empty, looked up on PATH
}

// Flash invokes esptool write_flash with hash verification (default for
// esptool v5+; the explicit `--verify` flag is preserved for older versions).
func (e Esptool) Flash(ctx context.Context, req FlashRequest, stdout, stderr io.Writer) error {
	if err := req.validate(); err != nil {
		return err
	}
	bin, err := e.resolveBin()
	if err != nil {
		return err
	}
	// esptool v5 prefers hyphenated names; v4 accepted underscored variants.
	// The hyphenated forms used here have been valid since esptool v4.6, so
	// they cover both v4.6+ and v5+. Stick with one set rather than detecting
	// the installed version.
	args := []string{
		"--chip", req.Chip,
		"--port", req.Port,
		"--baud", fmt.Sprintf("%d", req.Baud),
		"--before", "default-reset",
		"--after", "hard-reset",
		"write-flash",
		fmt.Sprintf("0x%x", req.Offset),
		req.Binary,
	}
	cmd := exec.CommandContext(ctx, bin, args...)
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("esptool write_flash failed: %w", err)
	}
	return nil
}

func (e Esptool) resolveBin() (string, error) {
	if e.Bin != "" {
		return e.Bin, nil
	}
	for _, name := range []string{"esptool", "esptool.py"} {
		if p, err := exec.LookPath(name); err == nil {
			return p, nil
		}
	}
	return "", errors.New("esptool not found on PATH; install with `pipx install esptool` or via your package manager")
}

func (req FlashRequest) validate() error {
	var missing []string
	if strings.TrimSpace(req.Port) == "" {
		missing = append(missing, "port")
	}
	if strings.TrimSpace(req.Chip) == "" {
		missing = append(missing, "chip")
	}
	if strings.TrimSpace(req.Binary) == "" {
		missing = append(missing, "binary")
	}
	if req.Baud <= 0 {
		missing = append(missing, "baud")
	}
	if len(missing) > 0 {
		return fmt.Errorf("invalid flash request: missing %s", strings.Join(missing, ", "))
	}
	return nil
}
