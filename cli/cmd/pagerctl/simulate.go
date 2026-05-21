package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"

	"github.com/pageros/pagerctl/internal/simulate"
)

const simulateUsage = `pagerctl simulate — render a remote app in the simulator

Usage:
  pagerctl simulate <url> [flags]

Spawns the PagerOS simulator preconfigured for direct mode against <url>.
The simulator window stays open until you close it.

Flags:
  --bin <path>   simulator binary (default: $PAGEROS_SIMULATOR_BIN, then
                 pageros-simulator on PATH)
  -h, --help     show this message

Examples:
  pagerctl simulate http://localhost:8080/
  pagerctl simulate https://app.example.com/
`

func runSimulate(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("simulate", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, simulateUsage) }

	bin := fs.String("bin", "", "")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}

	pos := fs.Args()
	if len(pos) == 0 {
		fmt.Fprint(stderr, simulateUsage)
		return errors.New("missing <url> argument")
	}
	if len(pos) > 1 {
		return fmt.Errorf("unexpected extra arguments: %v", pos[1:])
	}

	opts := simulate.Options{
		URL: pos[0],
		Bin: *bin,
	}
	return simulate.Run(ctx, opts, simulate.SystemLauncher{}, stdout, stderr)
}
