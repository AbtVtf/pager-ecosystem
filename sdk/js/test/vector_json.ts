// Float-preserving JSON loader for PROTO-003 vector descriptors.
//
// Standard `JSON.parse` collapses `12` and `12.0` to the same JS Number,
// which loses the float/integer distinction the descriptors rely on.
// This loader is a hand-rolled JSON parser that recognises numeric
// literals carrying `.` or `e`/`E` and wraps them in `CborFloat` so the
// canonical encoder picks the right CBOR major type.
//
// Coverage is what the descriptors need (object, array, string, number,
// boolean, null) — strict JSON, no JSONC, no tolerance for trailing
// commas. Mirrors RFC 8259 grammar.

import { CborFloat } from "../src/codec.js";

export function parseFloatPreservingJson(text: string): unknown {
  const parser = new Parser(text);
  parser.skipWs();
  const value = parser.parseValue();
  parser.skipWs();
  if (!parser.atEnd()) {
    throw new SyntaxError(`trailing content at position ${parser.pos}`);
  }
  return value;
}

class Parser {
  pos = 0;
  constructor(readonly text: string) {}

  atEnd(): boolean {
    return this.pos >= this.text.length;
  }

  peek(): string {
    return this.text[this.pos] ?? "";
  }

  skipWs(): void {
    while (this.pos < this.text.length) {
      const c = this.text[this.pos]!;
      if (c === " " || c === "\t" || c === "\n" || c === "\r") this.pos += 1;
      else break;
    }
  }

  expect(c: string): void {
    if (this.peek() !== c) {
      throw new SyntaxError(`expected ${JSON.stringify(c)} at position ${this.pos}`);
    }
    this.pos += 1;
  }

  parseValue(): unknown {
    this.skipWs();
    const c = this.peek();
    if (c === "{") return this.parseObject();
    if (c === "[") return this.parseArray();
    if (c === "\"") return this.parseString();
    if (c === "t" || c === "f") return this.parseBoolean();
    if (c === "n") return this.parseNull();
    if (c === "-" || (c >= "0" && c <= "9")) return this.parseNumber();
    throw new SyntaxError(`unexpected character ${JSON.stringify(c)} at position ${this.pos}`);
  }

  parseObject(): Record<string, unknown> {
    this.expect("{");
    const out: Record<string, unknown> = {};
    this.skipWs();
    if (this.peek() === "}") { this.pos += 1; return out; }
    while (true) {
      this.skipWs();
      const key = this.parseString();
      this.skipWs();
      this.expect(":");
      const value = this.parseValue();
      out[key] = value;
      this.skipWs();
      const c = this.peek();
      if (c === ",") { this.pos += 1; continue; }
      if (c === "}") { this.pos += 1; return out; }
      throw new SyntaxError(`expected ',' or '}' at position ${this.pos}`);
    }
  }

  parseArray(): unknown[] {
    this.expect("[");
    const out: unknown[] = [];
    this.skipWs();
    if (this.peek() === "]") { this.pos += 1; return out; }
    while (true) {
      const v = this.parseValue();
      out.push(v);
      this.skipWs();
      const c = this.peek();
      if (c === ",") { this.pos += 1; continue; }
      if (c === "]") { this.pos += 1; return out; }
      throw new SyntaxError(`expected ',' or ']' at position ${this.pos}`);
    }
  }

  parseString(): string {
    this.expect("\"");
    let out = "";
    while (true) {
      if (this.pos >= this.text.length) {
        throw new SyntaxError("unterminated string");
      }
      const c = this.text[this.pos]!;
      this.pos += 1;
      if (c === "\"") return out;
      if (c === "\\") {
        if (this.pos >= this.text.length) {
          throw new SyntaxError("dangling escape");
        }
        const esc = this.text[this.pos]!;
        this.pos += 1;
        switch (esc) {
          case "\"": out += "\""; break;
          case "\\": out += "\\"; break;
          case "/": out += "/"; break;
          case "b": out += "\b"; break;
          case "f": out += "\f"; break;
          case "n": out += "\n"; break;
          case "r": out += "\r"; break;
          case "t": out += "\t"; break;
          case "u": {
            if (this.pos + 4 > this.text.length) {
              throw new SyntaxError("truncated \\u escape");
            }
            const hex = this.text.slice(this.pos, this.pos + 4);
            this.pos += 4;
            const code = parseInt(hex, 16);
            if (Number.isNaN(code)) {
              throw new SyntaxError(`invalid \\u escape: ${hex}`);
            }
            out += String.fromCharCode(code);
            break;
          }
          default:
            throw new SyntaxError(`unknown escape: \\${esc}`);
        }
        continue;
      }
      out += c;
    }
  }

  parseBoolean(): boolean {
    if (this.text.startsWith("true", this.pos)) {
      this.pos += 4;
      return true;
    }
    if (this.text.startsWith("false", this.pos)) {
      this.pos += 5;
      return false;
    }
    throw new SyntaxError(`expected boolean at position ${this.pos}`);
  }

  parseNull(): null {
    if (this.text.startsWith("null", this.pos)) {
      this.pos += 4;
      return null;
    }
    throw new SyntaxError(`expected null at position ${this.pos}`);
  }

  parseNumber(): number | CborFloat | bigint {
    const start = this.pos;
    if (this.peek() === "-") this.pos += 1;
    while (this.pos < this.text.length) {
      const c = this.text[this.pos]!;
      if (c >= "0" && c <= "9") { this.pos += 1; continue; }
      break;
    }
    let isFloat = false;
    if (this.peek() === ".") {
      isFloat = true;
      this.pos += 1;
      while (this.pos < this.text.length) {
        const c = this.text[this.pos]!;
        if (c >= "0" && c <= "9") { this.pos += 1; continue; }
        break;
      }
    }
    if (this.peek() === "e" || this.peek() === "E") {
      isFloat = true;
      this.pos += 1;
      if (this.peek() === "+" || this.peek() === "-") this.pos += 1;
      while (this.pos < this.text.length) {
        const c = this.text[this.pos]!;
        if (c >= "0" && c <= "9") { this.pos += 1; continue; }
        break;
      }
    }
    const token = this.text.slice(start, this.pos);
    if (token === "" || token === "-") {
      throw new SyntaxError(`invalid number at position ${start}`);
    }
    const value = Number(token);
    if (Number.isNaN(value)) {
      throw new SyntaxError(`invalid number token ${JSON.stringify(token)}`);
    }
    if (isFloat) return new CborFloat(value);
    if (!Number.isSafeInteger(value)) {
      // outside safe-integer range — fall back to BigInt to keep
      // round-trip fidelity for large uint vectors.
      try {
        return BigInt(token);
      } catch {
        return value;
      }
    }
    return value;
  }
}
