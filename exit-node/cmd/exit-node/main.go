// exit-node is the PagerOS LoRa <-> HTTPS bridge.
//
// EXIT-001 acceptance: binary loads YAML config, opens LoRa device, prints
// status. Subsequent EXIT-* tasks layer in the RX/TX loop and HTTPS proxy.
package main

import (
	"flag"
	"fmt"
	"log"
	"os"

	"github.com/pageros/pager-ecosystem/exit-node/internal/config"
	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
)

const version = "0.1.0-dev"

func main() {
	var (
		cfgPath     = flag.String("config", "/etc/pager-ecosystem/exit-node.yaml", "path to YAML config")
		showVersion = flag.Bool("version", false, "print version and exit")
		statusOnly  = flag.Bool("status", false, "open the device, print status, then exit")
	)
	flag.Parse()

	if *showVersion {
		fmt.Printf("exit-node %s\n", version)
		return
	}

	cfg, err := config.Load(*cfgPath)
	if err != nil {
		log.Fatalf("config: %v", err)
	}
	fmt.Printf("exit-node %s — node_name=%s region=%s\n", version, cfg.NodeName, cfg.LoRa.Region)
	fmt.Printf("  config: %s\n", *cfgPath)
	fmt.Printf("  rate_limit: %d req/min/device\n", cfg.RateLimit.PerDevicePerMin)
	fmt.Printf("  stats: enabled=%t\n", cfg.Stats.Enabled)

	dev, err := lora.Open(cfg.LoRa.Port, cfg.LoRa.BaudRate)
	if err != nil {
		log.Fatalf("lora: %v", err)
	}
	defer dev.Close()

	st := dev.Status()
	fmt.Printf("  lora: port=%s baud=%d opened_at=%s\n",
		st.Port, st.BaudRate, st.OpenedAt.Format("2006-01-02T15:04:05Z07:00"))
	fmt.Println("status: ok (EXIT-001 scope — RX/TX loop arrives with EXIT-002)")

	if *statusOnly {
		return
	}

	// EXIT-001 is skeleton-only. Without the RX/TX loop (EXIT-002) we have
	// nothing useful to do after printing status, so exit cleanly instead of
	// idling.
	fmt.Fprintln(os.Stderr, "no RX/TX loop yet (EXIT-002); exiting.")
}
