package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"time"

	"github.com/pageros/pagerctl/internal/publish"
)

const publishUsage = `pagerctl publish — register an app with the Marketplace

Usage:
  pagerctl publish [flags] [path/to/manifest.yaml]

Flags:
  --registry <url>     Marketplace API base URL
                       (default: https://api.market.pageros.org)
  --listing-base <url> Public listing URL prefix
                       (default: https://market.pageros.org/apps/)
  --skip-prompt        Do not wait for Enter after printing the TXT record;
                       useful for CI where DNS is already configured.
  --verify-delay <d>   Wait this long after the prompt before calling verify
                       (e.g. 10s). Useful with --skip-prompt when the TXT
                       record has just been added.
  -h, --help           show this message
`

func runPublish(ctx context.Context, args []string, stdin io.Reader, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("publish", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, publishUsage) }

	registry := fs.String("registry", "", "")
	listingBase := fs.String("listing-base", "", "")
	skipPrompt := fs.Bool("skip-prompt", false, "")
	verifyDelay := fs.Duration("verify-delay", 0, "")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}

	pos := fs.Args()
	if len(pos) > 1 {
		return fmt.Errorf("unexpected extra arguments: %v", pos[1:])
	}
	manifestPath := "manifest.yaml"
	if len(pos) == 1 {
		manifestPath = pos[0]
	}

	opts := publish.Options{
		ManifestPath: manifestPath,
		RegistryURL:  *registry,
		ListingBase:  *listingBase,
		SkipPrompt:   *skipPrompt,
		VerifyDelay:  *verifyDelay,
		Now:          time.Now,
	}

	_, err := publish.Run(ctx, opts, nil, stdin, stdout)
	return err
}
