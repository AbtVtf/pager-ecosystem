package publish

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const minimalManifest = `id: notes.mafu.dev
name: Notes
description: A simple notepad.
icon: https://notes.app/icon.png
url: https://notes.app/
categories: [productivity]
maintainer:
  name: Jane Doe
  contact: jane@example.com
version: 1
`

func writeManifest(t *testing.T, body string) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "manifest.yaml")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("write manifest: %v", err)
	}
	return path
}

func TestLoadManifest_Minimal(t *testing.T) {
	path := writeManifest(t, minimalManifest)
	m, err := LoadManifest(path)
	if err != nil {
		t.Fatalf("LoadManifest: %v", err)
	}
	if m.ID != "notes.mafu.dev" {
		t.Errorf("ID = %q, want notes.mafu.dev", m.ID)
	}
	if m.Version != 1 {
		t.Errorf("Version = %d, want 1", m.Version)
	}
	if m.Maintainer.Contact != "jane@example.com" {
		t.Errorf("Maintainer.Contact = %q", m.Maintainer.Contact)
	}
	if len(m.Categories) != 1 || m.Categories[0] != "productivity" {
		t.Errorf("Categories = %v", m.Categories)
	}
	if err := m.Validate(); err != nil {
		t.Fatalf("Validate: %v", err)
	}
}

func TestLoadManifest_RejectsUnknownKey(t *testing.T) {
	body := minimalManifest + "bogus_key: true\n"
	path := writeManifest(t, body)
	_, err := LoadManifest(path)
	if err == nil {
		t.Fatal("expected error for unknown key, got nil")
	}
	if !strings.Contains(err.Error(), "bogus_key") {
		t.Errorf("error %q should mention bogus_key", err)
	}
}

func TestLoadManifest_MissingFile(t *testing.T) {
	_, err := LoadManifest("/nope/does-not-exist.yaml")
	if err == nil {
		t.Fatal("expected error")
	}
	if !strings.Contains(err.Error(), "read manifest") {
		t.Errorf("error should be a read error, got %v", err)
	}
}

func TestValidate_RequiredFields(t *testing.T) {
	m := &Manifest{}
	err := m.Validate()
	var ve *ValidationError
	if !errors.As(err, &ve) {
		t.Fatalf("want ValidationError, got %T: %v", err, err)
	}
	want := map[string]bool{
		"id":                 true,
		"name":               true,
		"description":        true,
		"icon":               true,
		"url":                true,
		"maintainer.name":    true,
		"maintainer.contact": true,
		"categories":         true,
		"version":            true,
	}
	gotFields := map[string]bool{}
	for _, fe := range ve.Errors {
		gotFields[fe.Field] = true
	}
	for k := range want {
		if !gotFields[k] {
			t.Errorf("missing validation error for field %q", k)
		}
	}
}

func TestValidate_RejectsBadID(t *testing.T) {
	m := validManifest()
	m.ID = "NotReverseDNS" // uppercase + no dot
	err := m.Validate()
	var ve *ValidationError
	if !errors.As(err, &ve) {
		t.Fatalf("want ValidationError, got %v", err)
	}
	var found bool
	for _, fe := range ve.Errors {
		if fe.Field == "id" {
			found = true
		}
	}
	if !found {
		t.Errorf("expected id field error, got %v", ve.Errors)
	}
}

func TestValidate_RejectsNonHTTPURL(t *testing.T) {
	m := validManifest()
	m.URL = "ftp://example.com"
	err := m.Validate()
	var ve *ValidationError
	if !errors.As(err, &ve) {
		t.Fatalf("want ValidationError, got %v", err)
	}
	var found bool
	for _, fe := range ve.Errors {
		if fe.Field == "url" {
			found = true
		}
	}
	if !found {
		t.Errorf("expected url field error, got %v", ve.Errors)
	}
}

func TestHostFromURL(t *testing.T) {
	m := validManifest()
	m.URL = "https://notes.app:8443/path"
	host, err := m.HostFromURL()
	if err != nil {
		t.Fatalf("HostFromURL: %v", err)
	}
	if host != "notes.app" {
		t.Errorf("host = %q, want notes.app", host)
	}
}

func TestValidationError_Error(t *testing.T) {
	one := &ValidationError{Errors: []FieldError{{Field: "id", Message: "required"}}}
	if !strings.Contains(one.Error(), "id: required") {
		t.Errorf("single-error message wrong: %q", one.Error())
	}
	many := &ValidationError{Errors: []FieldError{
		{Field: "id", Message: "required"},
		{Field: "url", Message: "must be an absolute http(s) URL"},
	}}
	msg := many.Error()
	if !strings.Contains(msg, "2 errors") {
		t.Errorf("multi-error message should mention count: %q", msg)
	}
	if !strings.Contains(msg, "id: required") || !strings.Contains(msg, "url:") {
		t.Errorf("multi-error message missing fields: %q", msg)
	}
}

func validManifest() *Manifest {
	return &Manifest{
		ID:          "notes.mafu.dev",
		Name:        "Notes",
		Description: "A simple notepad.",
		Icon:        "https://notes.app/icon.png",
		URL:         "https://notes.app/",
		Categories:  []string{"productivity"},
		Maintainer:  Maintainer{Name: "Jane Doe", Contact: "jane@example.com"},
		Version:     1,
	}
}
