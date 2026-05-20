package main

import (
	"context"
	"fmt"
	"os"
)

var (
	version = "dev"
	commit  = "none"
	date    = "unknown"
)

const usage = `pagerctl — PagerOS CLI

Usage:
  pagerctl <command> [args]

Commands:
  init        Scaffold a new PagerOS app (see CLI-001, CLI-007)
  dev         Run local app server + simulator (CLI-002)
  simulate    Render a remote app in the simulator (CLI-003)
  publish     Publish an app to the Marketplace (CLI-004)
  flash       Flash firmware to attached device (CLI-005)
  link        Pair real device to local dev server (CLI-006)
  version     Show version information
  help        Show this message
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	switch os.Args[1] {
	case "version", "--version", "-v":
		fmt.Printf("pagerctl %s (commit %s, built %s)\n", version, commit, date)
	case "help", "--help", "-h":
		fmt.Print(usage)
	case "flash":
		if err := runFlash(context.Background(), os.Args[2:], os.Stdout, os.Stderr); err != nil {
			fmt.Fprintf(os.Stderr, "pagerctl flash: %v\n", err)
			os.Exit(1)
		}
	case "init", "dev", "simulate", "publish", "link":
		fmt.Fprintf(os.Stderr, "pagerctl: %q is not implemented yet\n", os.Args[1])
		os.Exit(1)
	default:
		fmt.Fprintf(os.Stderr, "pagerctl: unknown command %q\n", os.Args[1])
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
}
