// Package lora implements the PagerOS LoRa wire envelope codec.
//
// Wire format (SPEC.md §6.2.1):
//
//	┌────────────┬────────────┬──────────────┬─────────────┬─────────────┐
//	│ magic (2B) │ version(1B)│ type (1B)    │ msg_id (4B) │ payload     │
//	└────────────┴────────────┴──────────────┴─────────────┴─────────────┘
//
// magic   : 0x50 0x47 ("PG")
// version : protocol version (currently 1)
// type    : message type — see Type* constants
// msg_id  : random u32 (big-endian) used for ACK + response matching
// payload : opaque bytes (CBOR-encoded inner envelope for type=Request/Response)
package lora

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// MagicV1 is the on-wire magic for v1 PagerOS LoRa packets: ASCII "PG".
var MagicV1 = [2]byte{0x50, 0x47}

// HeaderSize is the fixed-size envelope header (magic+version+type+msg_id).
const HeaderSize = 8

// Type is the LoRa envelope message type byte.
type Type uint8

const (
	TypeRequest         Type = 0x01
	TypeResponse        Type = 0x02
	TypeAck             Type = 0x03
	TypeExitNodeAdvert  Type = 0x04
)

// IsKnown reports whether t is a type defined by v1 of the spec.
// Unknown types are valid on the wire (forward compat) but callers
// should drop them rather than process the payload.
func (t Type) IsKnown() bool {
	switch t {
	case TypeRequest, TypeResponse, TypeAck, TypeExitNodeAdvert:
		return true
	default:
		return false
	}
}

// Envelope is the decoded outer LoRa envelope.
type Envelope struct {
	Version uint8
	Type    Type
	MsgID   uint32
	Payload []byte
}

// Sentinel errors returned by Decode.
var (
	ErrShort    = errors.New("lora: envelope too short")
	ErrBadMagic = errors.New("lora: bad magic")
	// ErrUnknownType signals a type byte not defined in this version.
	// The envelope is still returned (header + payload) so callers can
	// log/inspect; per spec, callers SHOULD drop it.
	ErrUnknownType = errors.New("lora: unknown type")
)

// Encode serializes e to the LoRa wire format.
// Version must be non-zero. Payload may be nil or empty.
func Encode(e Envelope) ([]byte, error) {
	if e.Version == 0 {
		return nil, fmt.Errorf("lora: version must be non-zero")
	}
	buf := make([]byte, HeaderSize+len(e.Payload))
	buf[0] = MagicV1[0]
	buf[1] = MagicV1[1]
	buf[2] = e.Version
	buf[3] = byte(e.Type)
	binary.BigEndian.PutUint32(buf[4:8], e.MsgID)
	copy(buf[HeaderSize:], e.Payload)
	return buf, nil
}

// Decode parses a LoRa packet into an Envelope.
//
// On bad magic it returns ErrBadMagic and a zero Envelope.
// On unknown type it returns ErrUnknownType *and* the parsed envelope —
// callers should drop the packet but may use the header for diagnostics.
func Decode(b []byte) (Envelope, error) {
	if len(b) < HeaderSize {
		return Envelope{}, ErrShort
	}
	if b[0] != MagicV1[0] || b[1] != MagicV1[1] {
		return Envelope{}, ErrBadMagic
	}
	e := Envelope{
		Version: b[2],
		Type:    Type(b[3]),
		MsgID:   binary.BigEndian.Uint32(b[4:8]),
	}
	// Copy so callers can reuse the input buffer.
	if n := len(b) - HeaderSize; n > 0 {
		e.Payload = make([]byte, n)
		copy(e.Payload, b[HeaderSize:])
	}
	if !e.Type.IsKnown() {
		return e, ErrUnknownType
	}
	return e, nil
}
