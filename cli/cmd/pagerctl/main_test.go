package main

import "testing"

func TestUsageMentionsAllCommands(t *testing.T) {
	cmds := []string{"init", "dev", "simulate", "publish", "flash", "link", "version", "help"}
	for _, c := range cmds {
		if !contains(usage, c) {
			t.Errorf("usage missing command %q", c)
		}
	}
}

func contains(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
