// Package initapp implements the `pagerctl init <lang>` command.
//
// It scaffolds a runnable hello-world PagerOS app in the target language. The
// only language implemented at M0 is Python (PY-001 has shipped). JS lands
// when JS-001 ships; until then, `init js` fails with an actionable error.
package initapp

import (
	"embed"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"text/template"
)

//go:embed templates
var templatesFS embed.FS

// Languages supported by `pagerctl init`. Order matters — used in CLI help.
var Languages = []string{"python", "js"}

// Templates supported by `pagerctl init` (CLI-007). The keys are template
// slugs accepted on the CLI; the order is the display order in help text.
// Each entry must have a directory at `templates/<lang>/<slug>/` with at
// minimum `app.py.tmpl` and `README.md.tmpl`.
var Templates = []string{
	"hello",
	"form",
	"list-detail",
	"location",
	"nfc-counter",
	"chat",
	"push-reminder",
}

// Options is the parsed CLI input for `pagerctl init`.
type Options struct {
	Lang     string // "python" or "js"
	Template string // template slug from Templates; defaults to "hello"
	Name     string // project name; used as directory name and manifest.name
	AppID    string // manifest id; defaults to "<name>.example.com"-style placeholder
	Dir      string // parent directory (default: cwd)
	Force    bool   // overwrite an existing non-empty target directory
}

// Defaults applies sensible defaults to unset fields.
func (o Options) Defaults() (Options, error) {
	if o.Lang == "" {
		return o, errors.New("missing --lang (python or js)")
	}
	o.Lang = strings.ToLower(o.Lang)
	if o.Template == "" {
		o.Template = "hello"
	}
	o.Template = strings.ToLower(o.Template)
	if !templateKnown(o.Template) {
		return o, fmt.Errorf("unknown --template %q (supported: %s)",
			o.Template, strings.Join(Templates, ", "))
	}
	if o.Name == "" {
		o.Name = "hello-pageros"
	}
	if !nameRE.MatchString(o.Name) {
		return o, fmt.Errorf("invalid --name %q: must match %s", o.Name, nameRE.String())
	}
	if o.AppID == "" {
		// Reverse-DNS-style placeholder the developer is expected to edit.
		o.AppID = o.Name + ".example.com"
	}
	if o.Dir == "" {
		cwd, err := os.Getwd()
		if err != nil {
			return o, fmt.Errorf("resolve cwd: %w", err)
		}
		o.Dir = cwd
	}
	return o, nil
}

func templateKnown(name string) bool {
	for _, t := range Templates {
		if t == name {
			return true
		}
	}
	return false
}

// nameRE constrains project names to safe directory/identifier characters.
// Lowercase to avoid clashing with the manifest id rule when the user adopts
// it as the leftmost label of their app id.
var nameRE = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$`)

// Result reports what was created.
type Result struct {
	TargetDir string
	Files     []string
}

// Run scaffolds a project and returns a summary. stdout receives a short
// human-facing message; errors are returned, not printed.
func Run(opts Options, stdout io.Writer) (*Result, error) {
	opts, err := opts.Defaults()
	if err != nil {
		return nil, err
	}

	target := filepath.Join(opts.Dir, opts.Name)

	switch opts.Lang {
	case "python":
		// supported
	case "js":
		return nil, errors.New(
			"pagerctl init js: JS scaffold not yet implemented (waiting on JS-001). " +
				"Use `pagerctl init python` for now.",
		)
	default:
		return nil, fmt.Errorf("unknown language %q (supported: %s)",
			opts.Lang, strings.Join(Languages, ", "))
	}

	if err := ensureEmptyDir(target, opts.Force); err != nil {
		return nil, err
	}

	files, err := renderTemplates(opts, target)
	if err != nil {
		return nil, err
	}

	fmt.Fprintf(stdout, "Created %s app in %s\n", opts.Lang, target)
	fmt.Fprintln(stdout, "Files:")
	for _, f := range files {
		fmt.Fprintf(stdout, "  %s\n", f)
	}
	fmt.Fprintln(stdout, "\nNext:")
	fmt.Fprintf(stdout, "  cd %s\n", opts.Name)
	fmt.Fprintln(stdout, "  python -m venv .venv && source .venv/bin/activate")
	fmt.Fprintln(stdout, "  pip install -r requirements.txt")
	fmt.Fprintln(stdout, "  python app.py")

	return &Result{TargetDir: target, Files: files}, nil
}

// ensureEmptyDir creates target if missing, or rejects an existing non-empty
// directory unless --force is set.
func ensureEmptyDir(target string, force bool) error {
	info, err := os.Stat(target)
	if errors.Is(err, os.ErrNotExist) {
		return os.MkdirAll(target, 0o755)
	}
	if err != nil {
		return fmt.Errorf("stat %s: %w", target, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("%s exists and is not a directory", target)
	}
	entries, err := os.ReadDir(target)
	if err != nil {
		return fmt.Errorf("read %s: %w", target, err)
	}
	if len(entries) > 0 && !force {
		return fmt.Errorf("%s is not empty (use --force to overwrite)", target)
	}
	return nil
}

// templateData carries values into the embedded templates.
type templateData struct {
	Name  string
	AppID string
}

// renderTemplates renders the language's shared files plus the chosen
// template's app.py.tmpl + README.md.tmpl into target.
//
// Layout:
//
//   templates/<lang>/*.tmpl                  shared files (manifest, gitignore, requirements)
//   templates/<lang>/<template>/*.tmpl       per-template files (app.py, README)
//
// Per-template files win on name collision, so a template may override a
// shared file by including its own copy.
func renderTemplates(opts Options, target string) ([]string, error) {
	sharedRoot := "templates/" + opts.Lang
	tmplRoot := sharedRoot + "/" + opts.Template
	data := templateData{Name: opts.Name, AppID: opts.AppID}

	rendered := map[string]string{} // outName → source path (template wins over shared)

	renderOne := func(root, path string) error {
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		outName := outputFilename(rel)
		outPath := filepath.Join(target, outName)
		if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
			return err
		}
		raw, err := templatesFS.ReadFile(path)
		if err != nil {
			return err
		}
		tmpl, err := template.New(path).Option("missingkey=error").Parse(string(raw))
		if err != nil {
			return fmt.Errorf("parse %s: %w", path, err)
		}
		f, err := os.Create(outPath)
		if err != nil {
			return err
		}
		if err := tmpl.Execute(f, data); err != nil {
			f.Close()
			return fmt.Errorf("render %s: %w", path, err)
		}
		if err := f.Close(); err != nil {
			return err
		}
		rendered[outName] = path
		return nil
	}

	// 1) Shared files (top-level *.tmpl under templates/<lang>/, NOT recursive).
	sharedEntries, err := templatesFS.ReadDir(sharedRoot)
	if err != nil {
		return nil, fmt.Errorf("read shared %s: %w", sharedRoot, err)
	}
	for _, d := range sharedEntries {
		if d.IsDir() {
			continue
		}
		if err := renderOne(sharedRoot, sharedRoot+"/"+d.Name()); err != nil {
			return nil, err
		}
	}

	// 2) Per-template files (recursive). Template-specific files overwrite
	// shared files with the same on-disk name.
	err = fs.WalkDir(templatesFS, tmplRoot, func(path string, d fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if d.IsDir() {
			return nil
		}
		return renderOne(tmplRoot, path)
	})
	if err != nil {
		return nil, err
	}

	created := make([]string, 0, len(rendered))
	for k := range rendered {
		created = append(created, k)
	}
	return created, nil
}

func outputFilename(rel string) string {
	switch rel {
	case "gitignore.tmpl":
		return ".gitignore"
	}
	return strings.TrimSuffix(rel, ".tmpl")
}
