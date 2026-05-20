package lora

import (
	"errors"
	"fmt"
)

// AdvertVersion is the current exit-node-advertise payload format version.
//
// See protocol/exit-node-advertise.md for the wire layout.
const AdvertVersion uint8 = 1

// AdvertSize is the minimum size of an advert payload in bytes (ver+pubkey+bw+load).
// Decoders accept longer payloads (forward-compat) but never shorter.
const AdvertSize = 1 + 32 + 1 + 1

// BWClass enumerates the internet-bandwidth class an Exit Node advertises.
type BWClass uint8

const (
	BWUnknown BWClass = 0
	BWLow     BWClass = 1
	BWMedium  BWClass = 2
	BWHigh    BWClass = 3
	BWGigabit BWClass = 4
)

// Advert is the decoded exit-node-advertise payload.
type Advert struct {
	Pubkey    [32]byte
	BWClass   BWClass
	Load      uint8
}

// Errors returned by DecodeAdvert.
var (
	ErrAdvertShort      = errors.New("lora: advert payload too short")
	ErrAdvertVersion    = errors.New("lora: unsupported advert version")
)

// EncodeAdvert serializes a to the on-wire advert payload (35 bytes).
//
// The result is what goes in the LoRa envelope's Payload field when
// Type == TypeExitNodeAdvert.
func EncodeAdvert(a Advert) []byte {
	buf := make([]byte, AdvertSize)
	buf[0] = AdvertVersion
	copy(buf[1:33], a.Pubkey[:])
	buf[33] = byte(a.BWClass)
	buf[34] = a.Load
	return buf
}

// DecodeAdvert parses an advert payload.
//
// Trailing bytes beyond AdvertSize are ignored for forward-compat.
// Returns ErrAdvertShort if too short, ErrAdvertVersion on unknown version.
func DecodeAdvert(b []byte) (Advert, error) {
	if len(b) < AdvertSize {
		return Advert{}, fmt.Errorf("%w: got %d, want >= %d", ErrAdvertShort, len(b), AdvertSize)
	}
	if b[0] != AdvertVersion {
		return Advert{}, fmt.Errorf("%w: got %d, want %d", ErrAdvertVersion, b[0], AdvertVersion)
	}
	var a Advert
	copy(a.Pubkey[:], b[1:33])
	a.BWClass = BWClass(b[33])
	a.Load = b[34]
	return a, nil
}
