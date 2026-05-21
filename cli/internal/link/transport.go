package link

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"

	"go.bug.st/serial"
)

// Opener constructs a Conn for a named serial port. Abstracted so the
// orchestrator can be unit-tested against an in-memory transport.
type Opener interface {
	Open(port string, baud int) (Conn, error)
}

// SystemOpener opens a real OS serial port via go.bug.st/serial.
type SystemOpener struct{}

func (SystemOpener) Open(port string, baud int) (Conn, error) {
	mode := &serial.Mode{
		BaudRate: baud,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}
	p, err := serial.Open(port, mode)
	if err != nil {
		return nil, fmt.Errorf("open serial port %s: %w", port, err)
	}
	// Toggling DTR/RTS together is the canonical way to keep the ESP32-S3
	// from auto-resetting into the bootloader the moment we open the
	// port. Without it, every link attempt would reboot the device into
	// download mode and the provisioning console would never see our
	// first request.
	_ = p.SetDTR(false)
	_ = p.SetRTS(false)
	return newSerialConn(p), nil
}

// serialConn adapts a serial.Port to the Conn interface. It owns a
// background reader goroutine that splits the byte stream into lines so
// ReadLine can respect a per-call context deadline without blocking on
// the underlying read indefinitely.
type serialConn struct {
	port   serial.Port
	lines  chan []byte
	errs   chan error
	closed chan struct{}
	once   sync.Once
}

func newSerialConn(port serial.Port) *serialConn {
	c := &serialConn{
		port:   port,
		lines:  make(chan []byte, 8),
		errs:   make(chan error, 1),
		closed: make(chan struct{}),
	}
	// Short-but-nonzero read timeout so the scanner unblocks regularly
	// and notices Close() promptly. Per-call deadlines are enforced in
	// ReadLine via the supplied context.
	_ = port.SetReadTimeout(100 * time.Millisecond)
	go c.readLoop()
	return c
}

func (c *serialConn) readLoop() {
	scanner := scanLines(c.port)
	for {
		if !scanner.Scan() {
			err := scanner.Err()
			if err == nil {
				err = io.EOF
			}
			select {
			case c.errs <- err:
			case <-c.closed:
			}
			return
		}
		line := append([]byte(nil), scanner.Bytes()...)
		select {
		case c.lines <- line:
		case <-c.closed:
			return
		}
	}
}

func (c *serialConn) WriteLine(line []byte) error {
	if len(line) > MaxLine {
		return fmt.Errorf("line too long (%d > %d)", len(line), MaxLine)
	}
	buf := make([]byte, 0, len(line)+1)
	buf = append(buf, line...)
	buf = append(buf, '\n')
	if _, err := c.port.Write(buf); err != nil {
		return err
	}
	return c.port.Drain()
}

func (c *serialConn) ReadLine(ctx context.Context) ([]byte, error) {
	select {
	case line := <-c.lines:
		return line, nil
	case err := <-c.errs:
		return nil, err
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-c.closed:
		return nil, io.EOF
	}
}

func (c *serialConn) Close() error {
	var err error
	c.once.Do(func() {
		close(c.closed)
		err = c.port.Close()
	})
	return err
}

// MemConn is an in-memory Conn used by tests. Writes go into a captured
// buffer and reads pop lines off a programmable queue. A pre-loaded
// "script" of (request → response) pairs is the most common use:
//
//	mem := NewMemConn()
//	mem.Reply(`{"ok":true}`)
//	mem.Reply(`{"ok":true}`)
//	cl := NewClient(mem)
//	_ = cl.SetWiFi(ctx, "net", "pw")  // writes line 1, consumes reply 1
//	...
//	got := mem.Written()              // ["{\"cmd\":\"set-wifi\",...}"]
//
// MemConn is concurrency-safe so the orchestrator's Reboot grace-window
// goroutine can call Close from one goroutine while ReadLine is waiting
// in another.
type MemConn struct {
	mu      sync.Mutex
	written [][]byte
	replies chan []byte
	closed  chan struct{}
	closeOnce sync.Once
}

func NewMemConn() *MemConn {
	return &MemConn{
		replies: make(chan []byte, 16),
		closed:  make(chan struct{}),
	}
}

// Reply queues one line to be returned by the next ReadLine call.
func (m *MemConn) Reply(line string) {
	m.replies <- []byte(line)
}

// ReplyBytes queues a raw byte slice (no copy taken; do not mutate).
func (m *MemConn) ReplyBytes(line []byte) {
	m.replies <- line
}

// Written returns every line written through WriteLine, in order, with
// trailing newlines stripped. Safe to call concurrently with writes.
func (m *MemConn) Written() []string {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]string, 0, len(m.written))
	for _, w := range m.written {
		out = append(out, string(bytes.TrimRight(w, "\n")))
	}
	return out
}

// WrittenLen returns the count of writes (cheaper than Written when
// the test only needs to know progress).
func (m *MemConn) WrittenLen() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.written)
}

func (m *MemConn) WriteLine(line []byte) error {
	// Writes on a closed MemConn are intentionally still recorded so
	// tests can simulate a device that accepted bytes before resetting
	// (the firmware's USB stack buffers the write; the reset happens
	// after the OK is queued). The read side enforces EOF.
	cp := append([]byte(nil), line...)
	cp = append(cp, '\n')
	m.mu.Lock()
	m.written = append(m.written, cp)
	m.mu.Unlock()
	return nil
}

func (m *MemConn) ReadLine(ctx context.Context) ([]byte, error) {
	select {
	case line := <-m.replies:
		return line, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-m.closed:
		return nil, io.EOF
	}
}

func (m *MemConn) Close() error {
	m.closeOnce.Do(func() { close(m.closed) })
	return nil
}

// MemOpener returns the same MemConn from every Open call so tests can
// program replies up-front. The opened port name and baud are recorded
// for assertions.
type MemOpener struct {
	Conn    *MemConn
	Port    string
	Baud    int
	OpenErr error
}

func (o *MemOpener) Open(port string, baud int) (Conn, error) {
	if o.OpenErr != nil {
		return nil, o.OpenErr
	}
	o.Port = port
	o.Baud = baud
	if o.Conn == nil {
		o.Conn = NewMemConn()
	}
	return o.Conn, nil
}

// Compile-time interface assertions: keep MemConn and serialConn in
// lockstep with Conn so a missing method shows up as a build error in
// this package rather than at the first test run.
var (
	_ Conn = (*MemConn)(nil)
	_ Conn = (*serialConn)(nil)
)

// ErrAlreadyClosed is returned when a duplicate Close races with another
// caller. Most consumers can ignore it.
var ErrAlreadyClosed = errors.New("conn already closed")
