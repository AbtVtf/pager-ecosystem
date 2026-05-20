package publish

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"time"
)

// Options is the parsed CLI input for `pagerctl publish`.
type Options struct {
	ManifestPath string        // path to manifest.yaml
	RegistryURL  string        // e.g. https://api.market.pageros.org
	ListingBase  string        // template for the public listing URL
	SkipPrompt   bool          // when true, do not pause for DNS TXT publication
	VerifyDelay  time.Duration // sleep between TXT publish and verify (default 5s)
	Now          func() time.Time
}

// Defaults applies PagerOS defaults to unset fields.
func (o Options) Defaults() Options {
	if o.RegistryURL == "" {
		o.RegistryURL = "https://api.market.pageros.org"
	}
	if o.ListingBase == "" {
		o.ListingBase = "https://market.pageros.org/apps/"
	}
	if o.Now == nil {
		o.Now = time.Now
	}
	return o
}

// Result is the public outcome of a successful publish.
type Result struct {
	AppID      string
	ListingURL string
	Record     *AppRecord
}

// Run executes the publish flow.
//
// stdin is used to wait for the developer to confirm the TXT record is live
// (skipped when --skip-prompt is set or stdin is nil). stdout receives progress
// lines; stderr is left to the caller (errors are returned, not printed).
func Run(ctx context.Context, opts Options, c *Client, stdin io.Reader, stdout io.Writer) (*Result, error) {
	opts = opts.Defaults()
	if c == nil {
		c = NewClient(opts.RegistryURL)
	}

	manifest, err := LoadManifest(opts.ManifestPath)
	if err != nil {
		return nil, err
	}
	if err := manifest.Validate(); err != nil {
		return nil, err
	}
	host, err := manifest.HostFromURL()
	if err != nil {
		return nil, fmt.Errorf("manifest url: %w", err)
	}

	fmt.Fprintf(stdout, "Manifest %s validated locally (id=%s, version=%d, host=%s).\n",
		opts.ManifestPath, manifest.ID, manifest.Version, host)

	fmt.Fprintf(stdout, "Requesting DNS TXT challenge from %s ...\n", c.BaseURL)
	ch, err := c.CreateChallenge(ctx, manifest.ID, manifest.URL)
	if err != nil {
		return nil, fmt.Errorf("create challenge: %w", explainAPIError(err))
	}
	fmt.Fprintf(stdout, "\nPublish this TXT record before continuing:\n\n")
	fmt.Fprintf(stdout, "    %s.  IN  TXT  %q\n\n", ch.TxtName, ch.TxtValue)
	fmt.Fprintf(stdout, "Challenge expires at %s.\n", ch.ExpiresAt.UTC().Format(time.RFC3339))

	if !opts.SkipPrompt {
		if err := waitForConfirmation(ctx, stdin, stdout); err != nil {
			return nil, err
		}
	}
	if opts.VerifyDelay > 0 {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(opts.VerifyDelay):
		}
	}

	fmt.Fprintln(stdout, "Verifying DNS TXT record ...")
	verified, err := c.VerifyChallenge(ctx, ch.ID)
	if err != nil {
		return nil, fmt.Errorf("verify challenge: %w", explainAPIError(err))
	}
	if verified.VerifiedAt == nil {
		return nil, fmt.Errorf("challenge %s did not enter verified state", verified.ID)
	}
	fmt.Fprintln(stdout, "DNS TXT challenge verified.")

	fmt.Fprintln(stdout, "Registering app with the Marketplace ...")
	record, err := c.RegisterApp(ctx, manifest, verified.Token)
	if err != nil {
		return nil, fmt.Errorf("register app: %w", explainAPIError(err))
	}
	listing := opts.ListingBase + record.Manifest.ID
	fmt.Fprintf(stdout, "\nPublished %s\n  listing: %s\n", record.Manifest.ID, listing)
	return &Result{AppID: record.Manifest.ID, ListingURL: listing, Record: record}, nil
}

// explainAPIError converts marketplace 400-with-fields errors into the same
// shape as local ValidationError so the CLI can present a unified message.
func explainAPIError(err error) error {
	var apiErr *APIError
	if errors.As(err, &apiErr) && apiErr.IsValidationFailure() {
		return &ValidationError{Errors: apiErr.Fields}
	}
	return err
}

func waitForConfirmation(ctx context.Context, stdin io.Reader, stdout io.Writer) error {
	if stdin == nil {
		return nil
	}
	fmt.Fprint(stdout, "Press Enter once the TXT record is live (or Ctrl-C to abort) ...")
	reader := bufio.NewReader(stdin)
	done := make(chan error, 1)
	go func() {
		_, err := reader.ReadString('\n')
		if err != nil && !errors.Is(err, io.EOF) {
			done <- err
			return
		}
		done <- nil
	}()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case err := <-done:
		fmt.Fprintln(stdout)
		if err != nil {
			return err
		}
		return nil
	}
}
