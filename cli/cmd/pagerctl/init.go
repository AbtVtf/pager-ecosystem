package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"strings"

	"github.com/pageros/pagerctl/internal/initapp"
)

const initUsage = `pagerctl init — scaffold a new PagerOS app

Usage:
  pagerctl init <lang> [flags]

Languages:
  python   Hello-world app using the pageros Python SDK.
  js       (pending JS-001 — not yet supported.)

Flags:
  --name <name>    Project name; used as directory name (default: hello-pageros)
  --app-id <id>    Reverse-DNS app id for the manifest
                   (default: <name>.example.com — edit before publishing)
  --dir <path>     Parent directory (default: cwd)
  --force          Scaffold into an existing non-empty directory
  -h, --help       show this message
`

func runInit(args []string, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("init", flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, initUsage) }

	name := fs.String("name", "", "")
	appID := fs.String("app-id", "", "")
	dir := fs.String("dir", "", "")
	force := fs.Bool("force", false, "")

	// Positional <lang> must come first; pull it out before flag parsing so the
	// developer can write the natural form `pagerctl init python --name foo`.
	if len(args) == 0 {
		fmt.Fprint(stderr, initUsage)
		return errors.New("missing <lang> argument")
	}
	if args[0] == "-h" || args[0] == "--help" {
		fmt.Fprint(stderr, initUsage)
		return nil
	}
	lang := strings.ToLower(args[0])
	if err := fs.Parse(args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if len(fs.Args()) > 0 {
		return fmt.Errorf("unexpected extra arguments: %v", fs.Args())
	}

	opts := initapp.Options{
		Lang:  lang,
		Name:  *name,
		AppID: *appID,
		Dir:   *dir,
		Force: *force,
	}
	_, err := initapp.Run(opts, stdout)
	return err
}
