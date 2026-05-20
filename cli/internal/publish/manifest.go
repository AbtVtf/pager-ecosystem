// Package publish implements the `pagerctl publish` command.
//
// The flow follows SPEC §10.3:
//  1. Read + parse the local YAML manifest.
//  2. Open a DNS TXT challenge with the Marketplace (MKT-003).
//  3. Ask the developer to publish the TXT record, then verify it.
//  4. Register the app via POST /apps with the challenge token (MKT-002).
//  5. Print the app id and its public listing URL.
package publish

import (
	"errors"
	"fmt"
	"net/url"
	"os"
	"regexp"
	"strings"

	"gopkg.in/yaml.v3"
)

// Manifest is the wire shape submitted to the Marketplace. It mirrors the
// pydantic “Manifest“ model in marketplace/api/pageros_marketplace/manifest.py.
//
// We keep validation here intentionally lightweight: the Marketplace is the
// source of truth and will reject anything we miss with structured field errors.
// What we do here is catch the obvious mistakes early so developers don't burn
// a DNS round-trip on a typo.
type Manifest struct {
	ID             string     `yaml:"id" json:"id"`
	Name           string     `yaml:"name" json:"name"`
	Description    string     `yaml:"description" json:"description"`
	Icon           string     `yaml:"icon" json:"icon"`
	URL            string     `yaml:"url" json:"url"`
	Pubkey         string     `yaml:"pubkey,omitempty" json:"pubkey,omitempty"`
	Permissions    []string   `yaml:"permissions,omitempty" json:"permissions,omitempty"`
	LoraCompatible bool       `yaml:"lora_compatible,omitempty" json:"lora_compatible,omitempty"`
	MultiDevice    bool       `yaml:"multi_device,omitempty" json:"multi_device,omitempty"`
	DonateURL      string     `yaml:"donate_url,omitempty" json:"donate_url,omitempty"`
	Categories     []string   `yaml:"categories" json:"categories"`
	Maintainer     Maintainer `yaml:"maintainer" json:"maintainer"`
	Version        int        `yaml:"version" json:"version"`
}

type Maintainer struct {
	Name    string `yaml:"name" json:"name"`
	Contact string `yaml:"contact" json:"contact"`
}

// LoadManifest reads and parses a YAML manifest from disk.
func LoadManifest(path string) (*Manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read manifest: %w", err)
	}
	var m Manifest
	dec := yaml.NewDecoder(strings.NewReader(string(data)))
	dec.KnownFields(true) // reject typo'd top-level keys
	if err := dec.Decode(&m); err != nil {
		return nil, fmt.Errorf("parse manifest %s: %w", path, err)
	}
	return &m, nil
}

// idPattern matches the reverse-DNS id rule from SPEC §10.2.
var idPattern = regexp.MustCompile(`^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$`)

// FieldError describes a specific manifest field that failed local validation.
// It mirrors the wire shape that the Marketplace returns on a 400 so the CLI
// can present a uniform error format whether the failure was caught here or
// server-side.
type FieldError struct {
	Field   string `json:"field"`
	Message string `json:"message"`
}

func (e FieldError) Error() string { return e.Field + ": " + e.Message }

// ValidationError is the top-level error returned for invalid manifests.
type ValidationError struct {
	Errors []FieldError
}

func (e *ValidationError) Error() string {
	switch len(e.Errors) {
	case 0:
		return "manifest invalid"
	case 1:
		return "manifest invalid: " + e.Errors[0].Error()
	default:
		var b strings.Builder
		fmt.Fprintf(&b, "manifest invalid (%d errors):", len(e.Errors))
		for _, fe := range e.Errors {
			b.WriteString("\n  - ")
			b.WriteString(fe.Error())
		}
		return b.String()
	}
}

// Validate performs cheap local checks. It is intentionally permissive — the
// canonical schema lives in the Marketplace and will catch anything missed
// here. The point is fast feedback on common typos.
func (m *Manifest) Validate() error {
	var errs []FieldError
	require := func(field, value string) {
		if strings.TrimSpace(value) == "" {
			errs = append(errs, FieldError{Field: field, Message: "required"})
		}
	}
	require("id", m.ID)
	require("name", m.Name)
	require("description", m.Description)
	require("icon", m.Icon)
	require("url", m.URL)
	require("maintainer.name", m.Maintainer.Name)
	require("maintainer.contact", m.Maintainer.Contact)

	if m.ID != "" && !idPattern.MatchString(m.ID) {
		errs = append(errs, FieldError{
			Field:   "id",
			Message: "must be reverse-DNS style (e.g. notes.mafu.dev)",
		})
	}
	if m.Version < 1 {
		errs = append(errs, FieldError{
			Field:   "version",
			Message: "must be >= 1",
		})
	}
	if len(m.Categories) == 0 {
		errs = append(errs, FieldError{
			Field:   "categories",
			Message: "at least one category is required",
		})
	}
	for _, name := range []struct {
		field string
		value string
	}{
		{"url", m.URL},
		{"icon", m.Icon},
	} {
		if name.value != "" && !isHTTPURL(name.value) {
			errs = append(errs, FieldError{
				Field:   name.field,
				Message: "must be an absolute http(s) URL",
			})
		}
	}
	if m.DonateURL != "" && !isHTTPURL(m.DonateURL) {
		errs = append(errs, FieldError{
			Field:   "donate_url",
			Message: "must be an absolute http(s) URL",
		})
	}
	if len(errs) > 0 {
		return &ValidationError{Errors: errs}
	}
	return nil
}

func isHTTPURL(s string) bool {
	u, err := url.Parse(s)
	if err != nil {
		return false
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return false
	}
	return u.Host != ""
}

// HostFromURL returns the bare hostname from the manifest URL — used to tell
// the developer where to publish the DNS TXT record.
func (m *Manifest) HostFromURL() (string, error) {
	u, err := url.Parse(m.URL)
	if err != nil {
		return "", err
	}
	if u.Host == "" {
		return "", errors.New("manifest url has no host")
	}
	return u.Hostname(), nil
}
