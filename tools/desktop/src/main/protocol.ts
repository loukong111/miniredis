import type { RespValue } from "../shared/contracts";

const MAX_BUFFER = 64 * 1024 * 1024;
const MAX_ARRAY = 100_000;
const MAX_DEPTH = 64;
const INCOMPLETE = Symbol("incomplete");
type Parsed = { value: RespValue; end: number } | typeof INCOMPLETE;

export function encodeCommand(parts: string[]): Buffer {
  const chunks = [Buffer.from(`*${parts.length}\r\n`)];
  for (const part of parts) {
    const bytes = Buffer.from(part);
    chunks.push(
      Buffer.from(`$${bytes.length}\r\n`),
      bytes,
      Buffer.from("\r\n"),
    );
  }
  return Buffer.concat(chunks);
}

export class RespDecoder {
  private buffer = Buffer.alloc(0);

  feed(data: Buffer): RespValue[] {
    if (this.buffer.length + data.length > MAX_BUFFER)
      throw new Error("响应超过 64 MiB 限制");
    this.buffer = Buffer.concat([this.buffer, data]);
    const values: RespValue[] = [];
    let position = 0;
    while (position < this.buffer.length) {
      const parsed = this.parse(position, 0);
      if (parsed === INCOMPLETE) break;
      values.push(parsed.value);
      position = parsed.end;
    }
    if (position) this.buffer = Buffer.from(this.buffer.subarray(position));
    return values;
  }

  private parse(start: number, depth: number): Parsed {
    if (depth > MAX_DEPTH) throw new Error("RESP 嵌套层数超过限制");
    if (start >= this.buffer.length) return INCOMPLETE;
    const marker = String.fromCharCode(this.buffer[start]);
    if (!"+-:$*".includes(marker)) throw new Error("收到非法 RESP 类型");
    const lineEnd = this.buffer.indexOf("\r\n", start);
    if (lineEnd < 0) {
      if (this.buffer.length - start > 4096) throw new Error("RESP 响应头过长");
      return INCOMPLETE;
    }
    if (lineEnd - start > 4096) throw new Error("RESP 响应头过长");
    const line = this.buffer.toString("utf8", start + 1, lineEnd);
    const next = lineEnd + 2;
    if (marker === "+" || marker === "-") {
      return {
        value: { type: marker === "+" ? "simple" : "error", value: line },
        end: next,
      };
    }
    if (!/^-?\d+$/.test(line)) throw new Error("RESP 数字格式错误");
    if (marker === ":")
      return { value: { type: "integer", value: line }, end: next };
    const count = Number(line);
    if (!Number.isSafeInteger(count) || count < -1)
      throw new Error("RESP 长度无效");
    if (count === -1) return { value: { type: "null" }, end: next };
    if (marker === "$") {
      if (count > MAX_BUFFER) throw new Error("RESP 字符串超过限制");
      if (next + count + 2 > this.buffer.length) return INCOMPLETE;
      if (
        this.buffer.toString("ascii", next + count, next + count + 2) !== "\r\n"
      ) {
        throw new Error("RESP 字符串结尾错误");
      }
      return {
        value: {
          type: "bulk",
          value: this.buffer.toString("utf8", next, next + count),
        },
        end: next + count + 2,
      };
    }
    if (count > MAX_ARRAY) throw new Error("RESP 数组超过限制");
    const values: RespValue[] = [];
    let position = next;
    for (let i = 0; i < count; i++) {
      const child = this.parse(position, depth + 1);
      if (child === INCOMPLETE) return INCOMPLETE;
      values.push(child.value);
      position = child.end;
    }
    return { value: { type: "array", value: values }, end: position };
  }
}
