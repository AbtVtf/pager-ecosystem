package lora

import (
	"fmt"
	"io"
	"os"
	"time"

	"go.bug.st/serial"
)

// Device is the minimal interface the exit-node uses to talk to a LoRa
// transceiver. EXIT-002 will expand this with a RX/TX loop; for EXIT-001 we
// only need open + status + close so the binary can prove it can reach the
// hardware.
type Device interface {
	io.ReadWriteCloser
	Status() Status
}

type Status struct {
	Port     string
	BaudRate int
	OpenedAt time.Time
}

// Open opens the configured serial port. It returns an error if the device
// file does not exist (caller surfaces this; EXIT-008 packaging targets a real
// USB LoRa dongle).
func Open(port string, baud int) (Device, error) {
	if _, err := os.Stat(port); err != nil {
		return nil, fmt.Errorf("lora: device %s not accessible: %w", port, err)
	}
	p, err := serial.Open(port, &serial.Mode{BaudRate: baud})
	if err != nil {
		return nil, fmt.Errorf("lora: open %s @ %d: %w", port, baud, err)
	}
	return &serialDevice{
		port:     p,
		status:   Status{Port: port, BaudRate: baud, OpenedAt: time.Now()},
	}, nil
}

type serialDevice struct {
	port   serial.Port
	status Status
}

func (d *serialDevice) Read(p []byte) (int, error)  { return d.port.Read(p) }
func (d *serialDevice) Write(p []byte) (int, error) { return d.port.Write(p) }
func (d *serialDevice) Close() error                { return d.port.Close() }
func (d *serialDevice) Status() Status              { return d.status }
