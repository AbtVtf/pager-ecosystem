package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"

	"github.com/pageros/pagerctl/internal/devcmd"
)

const devUsage = `pagerctl dev — run local app server + simulator

Usage:
  pagerctl dev [flags]

Spawns the user's PagerOS app via pageros.devserver (auto-reload on file
change) and opens the simulator pointing at it. One Ctrl+C stops both.

Flags:
  --app <path>        app entry (default: app.py in cwd)
  --host <host>       dev server bind host (default: 127.0.0.1)
  --port <n>          dev server bind port (default: 8000)
  --python <bin>      Python interpreter (default: $PAGEROS_PYTHON, then python3)
  --bin <path>        simulator binary (default: $PAGEROS_SIMULATOR_BIN, then
                      pageros-simulator on PATH)
  --no-simulator      skip launching the simulator
  -h, --help          show this message

Examples:
  pagerctl dev
  pagerctl dev --port 8001
  pagerctl dev --app examples/hello/app.py --python .venv/bin/python
`

func runDev(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("dev", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, devUsage) }

	app := fs.String("app", "", "")
	host := fs.String("host", "", "")
	port := fs.Int("port", 0, "")
	python := fs.String("python", "", "")
	bin := fs.String("bin", "", "")
	noSim := fs.Bool("no-simulator", false, "")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if pos := fs.Args(); len(pos) > 0 {
		return fmt.Errorf("unexpected extra arguments: %v", pos)
	}

	opts := devcmd.Options{
		App:          *app,
		Host:         *host,
		Port:         *port,
		Python:       *python,
		SimulatorBin: *bin,
		NoSimulator:  *noSim,
	}
	return devcmd.Run(ctx, opts, devcmd.SystemRunner{}, stdout, stderr)
}
