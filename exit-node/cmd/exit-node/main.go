// exit-node is the PagerOS LoRa <-> HTTPS bridge.
//
// EXIT-001 acceptance: binary loads YAML config, opens LoRa device, prints
// status. EXIT-005 adds the periodic discovery beacon (advertise) that
// emits exit-node-advertise packets per LORA-005.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"syscall"

	"github.com/pageros/pager-ecosystem/exit-node/internal/config"
	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
	"github.com/pageros/pager-ecosystem/exit-node/internal/stats"
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
	fmt.Printf("  advertise: enabled=%t interval=%s bw_class=%s\n",
		cfg.Advertise.Enabled, cfg.Advertise.Interval(), cfg.Advertise.BWClass)

	dev, err := lora.Open(cfg.LoRa.Port, cfg.LoRa.BaudRate)
	if err != nil {
		log.Fatalf("lora: %v", err)
	}
	defer dev.Close()

	st := dev.Status()
	fmt.Printf("  lora: port=%s baud=%d opened_at=%s\n",
		st.Port, st.BaudRate, st.OpenedAt.Format("2006-01-02T15:04:05Z07:00"))
	fmt.Println("status: ok")

	if *statusOnly {
		return
	}

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	// Counter source for the stats publisher. The RX/TX loop is not
	// wired into main yet (will land with EXIT-003 follow-up), so for
	// now publish an always-zero snapshot. The publisher still proves
	// the opt-in dashboard path is healthy and dashboards see the node
	// is alive (uptime increments).
	source := stats.SourceFunc(func() stats.LoopSnapshot { return stats.LoopSnapshot{} })

	var wg sync.WaitGroup

	if cfg.Stats.Enabled {
		pub, err := stats.NewPublisher(stats.Config{
			Enabled:  true,
			URL:      cfg.Stats.URL,
			NodeName: cfg.NodeName,
			Source:   source,
		})
		if err != nil {
			log.Fatalf("stats: %v", err)
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			if err := pub.Run(ctx); err != nil && !errors.Is(err, context.Canceled) {
				log.Printf("stats: publisher exited: %v", err)
			}
		}()
		fmt.Printf("  stats: publishing to %s\n", cfg.Stats.URL)
	}

	if cfg.Advertise.Enabled {
		if err := runAdvertise(ctx, cfg.Advertise, dev); err != nil && !errors.Is(err, context.Canceled) {
			cancel()
			wg.Wait()
			log.Fatalf("advertise: %v", err)
		}
		wg.Wait()
		return
	}

	// No advertiser configured and no RX/TX loop wired yet (EXIT-003 will
	// drive it). Block until signalled so operators can still ^C the
	// process during early integration.
	fmt.Fprintln(os.Stderr, "advertise: disabled; idling until signal (no RX/TX loop yet)")
	<-ctx.Done()
	wg.Wait()
}

// runAdvertise builds the LORA-005 Advertiser from config and runs it
// against the LoRa device until ctx is cancelled.
func runAdvertise(ctx context.Context, cfg config.Advertise, w writerWithStats) error {
	identity, err := cfg.DecodePubkey()
	if err != nil {
		return fmt.Errorf("identity_pubkey_hex: %w", err)
	}

	// Load is wired to 0 until EXIT-004 (rate limiter) exposes a real
	// load metric. Use an atomic so future code can plug in without
	// changing the Advertiser surface.
	var load atomic.Uint64

	adv, err := lora.NewAdvertiser(lora.AdvertiserConfig{
		Writer:   w,
		Identity: identity,
		BWClass:  lora.BWClass(cfg.BWClassValue()),
		Interval: cfg.Interval(),
		Load:     func() uint8 { return uint8(load.Load()) },
	})
	if err != nil {
		return err
	}
	fmt.Printf("  advertise: emitting beacons every %s (pubkey=%x…)\n",
		cfg.Interval(), identity[:4])
	return adv.Run(ctx)
}

// writerWithStats is the minimal interface main needs from the open
// LoRa device. Pulling it out makes runAdvertise unit-testable without
// touching real hardware.
type writerWithStats interface {
	Write(p []byte) (int, error)
}
