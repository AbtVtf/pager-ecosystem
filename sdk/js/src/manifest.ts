// App manifest generator (JS-010).
//
// Mirrors `sdk/python/pageros/manifest.py` — describes an app well
// enough to publish via the Marketplace (SPEC §10.2 / MKT-001).
//
// Fields are 1:1 with the Marketplace schema. Optional fields default
// to values that render as a minimal-but-valid manifest. Defaults stay
// close to "off" so a manifest doesn't accidentally advertise
// capabilities the app doesn't actually implement (LoRa, multi-device,
// permissions).
//
// Two emit modes:
//
//   - `manifestObject()` — plain JSON-serialisable object; pass to
//     anything that consumes JSON (the publish API, tests, the docs site).
//   - `manifestYaml()` — a minimal, human-readable YAML string the same
//     shape `pagerctl publish` writes to disk. Self-contained — no `yaml`
//     dependency.

/** Manifest maintainer block (SPEC §10.2). */
export interface Maintainer {
  name: string;
  contact: string;
}

/** Required + optional manifest fields, mirroring `Manifest` in the API. */
export interface AppManifestOptions {
  id: string;
  name: string;
  description: string;
  icon: string;
  url: string;
  maintainer: Maintainer;
  categories?: string[];
  version?: number;
  pubkey?: string;
  permissions?: string[];
  loraCompatible?: boolean;
  multiDevice?: boolean;
  donateUrl?: string;
}

/**
 * A PagerOS app, described well enough to publish.
 *
 * Construct with `new AppManifest({...})`. Two emit methods land the
 * manifest in the two formats the toolchain actually uses.
 */
export class AppManifest {
  readonly id: string;
  readonly name: string;
  readonly description: string;
  readonly icon: string;
  readonly url: string;
  readonly maintainer: Maintainer;
  readonly categories: string[];
  readonly version: number;
  readonly pubkey: string | null;
  readonly permissions: string[];
  readonly loraCompatible: boolean;
  readonly multiDevice: boolean;
  readonly donateUrl: string | null;

  constructor(opts: AppManifestOptions) {
    if (!opts.id) throw new RangeError("manifest.id is required");
    if (!opts.name) throw new RangeError("manifest.name is required");
    if (!opts.description) throw new RangeError("manifest.description is required");
    if (!opts.icon) throw new RangeError("manifest.icon is required");
    if (!opts.url) throw new RangeError("manifest.url is required");
    if (!opts.maintainer?.name || !opts.maintainer?.contact) {
      throw new RangeError("manifest.maintainer.{name,contact} are required");
    }
    if (opts.version !== undefined && (!Number.isInteger(opts.version) || opts.version < 1)) {
      throw new RangeError("manifest.version must be a positive integer");
    }
    this.id = opts.id;
    this.name = opts.name;
    this.description = opts.description;
    this.icon = opts.icon;
    this.url = opts.url;
    this.maintainer = { ...opts.maintainer };
    this.categories = [...(opts.categories ?? [])];
    this.version = opts.version ?? 1;
    this.pubkey = opts.pubkey ?? null;
    this.permissions = [...(opts.permissions ?? [])];
    this.loraCompatible = opts.loraCompatible ?? false;
    this.multiDevice = opts.multiDevice ?? false;
    this.donateUrl = opts.donateUrl ?? null;
  }

  /**
   * The manifest as a plain object. Field ordering follows SPEC §10.2
   * so generated output reads top-to-bottom the same way the spec
   * presents it. Optional-fields-with-default-empty values are omitted
   * to match the minimal-canonical shape the Python SDK emits.
   */
  manifestObject(): Record<string, unknown> {
    const out: Record<string, unknown> = {
      id: this.id,
      name: this.name,
      description: this.description,
      icon: this.icon,
      url: this.url,
    };
    if (this.pubkey) out.pubkey = this.pubkey;
    if (this.permissions.length) out.permissions = [...this.permissions];
    if (this.loraCompatible) out.lora_compatible = true;
    if (this.multiDevice) out.multi_device = true;
    if (this.donateUrl) out.donate_url = this.donateUrl;
    if (this.categories.length) out.categories = [...this.categories];
    out.maintainer = { name: this.maintainer.name, contact: this.maintainer.contact };
    out.version = this.version;
    return out;
  }

  /**
   * The manifest as YAML text — the format `pagerctl publish` reads
   * from disk and the Marketplace web UI shows on app pages.
   *
   * Hand-rolled emitter rather than pulling a YAML dep, because the
   * subset we need is tiny: top-level scalars, one nested map
   * (`maintainer`), and string lists. Strings that need quoting are
   * detected conservatively and double-quoted.
   */
  manifestYaml(): string {
    const obj = this.manifestObject();
    const lines: string[] = [];
    for (const [key, value] of Object.entries(obj)) {
      if (value === null || value === undefined) continue;
      if (Array.isArray(value)) {
        lines.push(`${key}: [${value.map(yamlScalar).join(", ")}]`);
      } else if (typeof value === "object") {
        lines.push(`${key}:`);
        for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
          lines.push(`  ${k}: ${yamlScalar(v)}`);
        }
      } else {
        lines.push(`${key}: ${yamlScalar(value)}`);
      }
    }
    return lines.join("\n") + "\n";
  }
}

const YAML_NEEDS_QUOTING = /^(?:|true|false|null|~|yes|no|on|off|-?\d+(?:\.\d+)?)$/i;

function yamlScalar(value: unknown): string {
  if (value === null || value === undefined) return "null";
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  const s = String(value);
  if (s === "" || YAML_NEEDS_QUOTING.test(s) || /[:#,\[\]{}\n"']/.test(s)) {
    return `"${s.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
  }
  return s;
}
