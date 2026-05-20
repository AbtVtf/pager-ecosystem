package proxy

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
)

// fakeTransceiver is the proxy-package-local equivalent of the loop's
// in-memory transceiver. Duplicated here (rather than exported from
// the lora package) to keep the integration test self-contained and to
// avoid leaking a test helper into the production API surface.
type fakeTransceiver struct {
	in     chan []byte
	out    chan []byte
	closed atomic.Bool
}

func newFakeTransceiver(buf int) *fakeTransceiver {
	return &fakeTransceiver{
		in:  make(chan []byte, buf),
		out: make(chan []byte, buf),
	}
}

func (f *fakeTransceiver) ReadPacket(ctx context.Context) ([]byte, error) {
	if f.closed.Load() {
		return nil, io.EOF
	}
	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	case b, ok := <-f.in:
		if !ok {
			return nil, io.EOF
		}
		return b, nil
	}
}

func (f *fakeTransceiver) WritePacket(ctx context.Context, p []byte) error {
	if f.closed.Load() {
		return io.ErrClosedPipe
	}
	select {
	case <-ctx.Done():
		return ctx.Err()
	case f.out <- append([]byte(nil), p...):
		return nil
	}
}

func (f *fakeTransceiver) Close() error {
	if f.closed.Swap(true) {
		return nil
	}
	close(f.in)
	return nil
}

// TestProxyEndToEndViaLoraLoop proves EXIT-003's acceptance: a request
// arrives on the LoRa wire, the proxy validates HTTPS + forwards to an
// upstream HTTPS server, and the upstream response comes back over the
// wire as a TypeResponse LoRa envelope. Exercises the full glue:
// LoRa outer envelope → fragmentation → reassembly → proxy → HTTPS →
// proxy → reassembled fragments → LoRa outer envelope.
func TestProxyEndToEndViaLoraLoop(t *testing.T) {
	var gotUpstreamBody atomic.Value
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		gotUpstreamBody.Store(b)
		w.Header().Set("Content-Type", ContentTypeInner)
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("from-app-server"))
	}))
	defer upstream.Close()

	innerEnv := lora.InnerEnvelope{
		To:    upstream.URL + "/save",
		From:  bytes.Repeat([]byte{0xAA}, lora.X25519KeyLen),
		Nonce: bytes.Repeat([]byte{0x02}, lora.InnerNonceLen),
		Sig:   bytes.Repeat([]byte{0xCD}, lora.SigLen),
		Body:  []byte("ciphertext+tag"),
	}
	innerRaw, err := lora.EncodeInner(innerEnv)
	if err != nil {
		t.Fatal(err)
	}

	ft := newFakeTransceiver(8)
	p := New(Config{HTTPClient: upstream.Client()})
	loop := lora.NewLoop(ft, p, lora.LoopOpts{FragmentChunkSize: 64})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	loopDone := make(chan error, 1)
	go func() { loopDone <- loop.Run(ctx) }()

	const msgID uint32 = 0xC0FFEE01
	frags, err := lora.SplitMessage(msgID, innerRaw, 64)
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range frags {
		fb, err := lora.EncodeFragmentBody(f)
		if err != nil {
			t.Fatal(err)
		}
		pkt, err := lora.Encode(lora.Envelope{Version: 1, Type: lora.TypeRequest, MsgID: msgID, Payload: fb})
		if err != nil {
			t.Fatal(err)
		}
		ft.in <- pkt
	}

	// Collect outbound packets until we've reassembled a full response.
	collected := [][]byte{}
	deadline := time.Now().Add(3 * time.Second)
	reasm := lora.NewReassembler()
	var responseBody []byte
	for time.Now().Before(deadline) {
		select {
		case pkt := <-ft.out:
			collected = append(collected, pkt)
			env, err := lora.Decode(pkt)
			if err != nil {
				t.Fatalf("decode outer: %v", err)
			}
			if env.Type != lora.TypeResponse {
				t.Errorf("response env type: got 0x%02x want 0x%02x", env.Type, lora.TypeResponse)
			}
			if env.MsgID != msgID {
				t.Errorf("response msg_id: got %08x want %08x", env.MsgID, msgID)
			}
			frag, err := lora.DecodeFragmentBody(env.Payload)
			if err != nil {
				t.Fatalf("decode frag: %v", err)
			}
			frag.MsgID = env.MsgID
			full, complete, err := reasm.Add(frag)
			if err != nil {
				t.Fatalf("reasm: %v", err)
			}
			if complete {
				responseBody = full
				break
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("timed out before response complete (got %d packets)", len(collected))
		}
		if responseBody != nil {
			break
		}
	}
	if responseBody == nil {
		t.Fatalf("no complete response within deadline (got %d packets)", len(collected))
	}
	if !bytes.Equal(responseBody, []byte("from-app-server")) {
		t.Errorf("reassembled response: got %q want %q", responseBody, "from-app-server")
	}
	if got := gotUpstreamBody.Load().([]byte); !bytes.Equal(got, innerRaw) {
		t.Errorf("upstream did not receive the inner envelope CBOR")
	}

	cancel()
	select {
	case err := <-loopDone:
		if err != nil && !errors.Is(err, context.Canceled) {
			t.Errorf("loop.Run returned %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Error("loop.Run did not exit after cancel")
	}
}
