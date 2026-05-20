package main

import "testing"

func TestParseOffset(t *testing.T) {
	cases := []struct {
		in   string
		want uint32
		err  bool
	}{
		{"0x10000", 0x10000, false},
		{"0X20000", 0x20000, false},
		{"10000", 10000, false},     // plain decimal
		{"abcd", 0xabcd, false},     // hex inferred from letters
		{"0xDEADBEEF", 0xdeadbeef, false},
		{"", 0, true},
		{"  ", 0, true},
		{"nothex", 0, true},
		{"0x", 0, true},
		{"0x100000000", 0, true}, // overflow uint32
	}
	for _, c := range cases {
		t.Run(c.in, func(t *testing.T) {
			got, err := parseOffset(c.in)
			if c.err {
				if err == nil {
					t.Fatalf("expected error for %q, got %d", c.in, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error for %q: %v", c.in, err)
			}
			if got != c.want {
				t.Errorf("parseOffset(%q) = 0x%x, want 0x%x", c.in, got, c.want)
			}
		})
	}
}
