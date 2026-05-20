package lora

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"time"
)

// LoadFunc reports the current Exit Node load (0=idle, 255=saturated).
// Implementations should be cheap — it's called once per advert.
type LoadFunc func() uint8

// AdvertiserConfig parameterises the periodic advertise loop.
type AdvertiserConfig struct {
	// Writer is the LoRa TX side. Each tick, Run writes one full LoRa
	// envelope containing the advert payload. Required.
	Writer io.Writer

	// Identity is the Exit Node's long-term Ed25519 public key.
	// Required (must not be all-zero).
	Identity [32]byte

	// BWClass is this node's advertised internet-bandwidth class.
	BWClass BWClass

	// Load is called every tick to read current load. If nil, load is 0.
	Load LoadFunc

	// Interval between adverts. Defaults to DefaultAdvertInterval (30 s)
	// if zero. Values outside [10s, 300s] are rejected per spec.
	Interval time.Duration

	// Clock — defaults to time.Now. Tests may inject a fake.
	Clock func() time.Time

	// MsgID — defaults to crypto/rand. Tests may inject deterministic IDs.
	MsgID func() uint32
}

// Advertiser broadcasts exit-node-advertise packets every Interval until
// the supplied context is cancelled (LORA-005).
type Advertiser struct {
	cfg AdvertiserConfig
}

// NewAdvertiser validates cfg and returns an Advertiser ready to Run.
func NewAdvertiser(cfg AdvertiserConfig) (*Advertiser, error) {
	if cfg.Writer == nil {
		return nil, errors.New("advertiser: Writer is required")
	}
	if cfg.Identity == ([32]byte{}) {
		return nil, errors.New("advertiser: Identity must be set")
	}
	if cfg.Interval == 0 {
		cfg.Interval = DefaultAdvertInterval
	}
	if cfg.Interval < 10*time.Second || cfg.Interval > 300*time.Second {
		return nil, fmt.Errorf("advertiser: Interval %s out of allowed range [10s,300s]", cfg.Interval)
	}
	if cfg.Clock == nil {
		cfg.Clock = time.Now
	}
	if cfg.MsgID == nil {
		cfg.MsgID = randomMsgID
	}
	return &Advertiser{cfg: cfg}, nil
}

// Run emits one advert immediately, then one every Interval until ctx is
// done. Returns ctx.Err() on shutdown, or the first non-nil write error.
//
// The ticker is driven by time.NewTicker; tests that need precise
// control should call SendOnce directly.
func (a *Advertiser) Run(ctx context.Context) error {
	if err := a.SendOnce(); err != nil {
		return err
	}
	t := time.NewTicker(a.cfg.Interval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-t.C:
			if err := a.SendOnce(); err != nil {
				return err
			}
		}
	}
}

// SendOnce builds and writes a single advert envelope. Exposed so tests
// (and the eventual exit-node service) can drive the schedule directly.
func (a *Advertiser) SendOnce() error {
	var load uint8
	if a.cfg.Load != nil {
		load = a.cfg.Load()
	}
	payload := EncodeAdvert(Advert{
		Pubkey:  a.cfg.Identity,
		BWClass: a.cfg.BWClass,
		Load:    load,
	})
	pkt, err := Encode(Envelope{
		Version: 1,
		Type:    TypeExitNodeAdvert,
		MsgID:   a.cfg.MsgID(),
		Payload: payload,
	})
	if err != nil {
		return fmt.Errorf("advertiser: encode: %w", err)
	}
	if _, err := a.cfg.Writer.Write(pkt); err != nil {
		return fmt.Errorf("advertiser: write: %w", err)
	}
	return nil
}

func randomMsgID() uint32 {
	var b [4]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand failure is essentially fatal on a Linux box;
		// fall back to nanosecond time to keep the loop alive.
		return uint32(time.Now().UnixNano())
	}
	return binary.BigEndian.Uint32(b[:])
}
