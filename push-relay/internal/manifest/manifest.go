// Package manifest looks up app manifests from the marketplace registry so the
// push relay can verify app signatures (PUSH-002, SPEC §6.6.2).
//
// The marketplace owns app identity (manifest id → registered pubkey); the
// relay only consumes that mapping. Implementations are pluggable behind
// Lookup so the production HTTP client can be swapped for a fake in tests.
//
// SPEC NOTE: SPEC §6.6.2 says the relay "verifies the app's Ed25519 signature
// against the app's registered manifest pubkey", and §10.2 documents the
// manifest pubkey as X25519 (for E2E encryption). The marketplace currently
// stores a single pubkey field; for PUSH-002 we treat the field returned by
// the registry as the Ed25519 signing key, byte-for-byte. Reconciling the
// "one key vs two keys" question is a marketplace concern, not a relay
// concern — whatever the registry returns is what the relay verifies against.
package manifest

import (
	"context"
	"crypto/ed25519"
	"errors"
)

// Errors returned by Lookup implementations.
var (
	// ErrAppNotFound means the app id is not registered in the marketplace.
	// Push handlers MUST map this to HTTP 403 (SPEC: "Rejects unknown apps
	// with 403.").
	ErrAppNotFound = errors.New("manifest: app not registered")

	// ErrLookupUnavailable means the lookup itself failed (network, decode,
	// upstream 5xx). Different from ErrAppNotFound: the app may still be
	// registered, we just could not determine it. Maps to HTTP 503 so a
	// well-behaved client retries with backoff.
	ErrLookupUnavailable = errors.New("manifest: lookup unavailable")
)

// Lookup resolves an app id to its registered Ed25519 signing public key.
type Lookup interface {
	Pubkey(ctx context.Context, appID string) (ed25519.PublicKey, error)
}

// LookupFunc adapts a plain function to the Lookup interface. Convenient for
// tests and for one-off injection at server-construction time.
type LookupFunc func(ctx context.Context, appID string) (ed25519.PublicKey, error)

func (f LookupFunc) Pubkey(ctx context.Context, appID string) (ed25519.PublicKey, error) {
	return f(ctx, appID)
}
