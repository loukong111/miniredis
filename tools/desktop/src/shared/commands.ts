import type { RespValue } from "./contracts";

export function parseScript(script: string): string[][] {
  if (
    typeof script !== "string" ||
    new TextEncoder().encode(script).length > 1024 * 1024
  )
    throw new Error("命令脚本超过 1 MiB 限制");
  const commands: string[][] = [];
  for (const [lineIndex, raw] of script.split(/\r?\n/).entries()) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const parts: string[] = [];
    let value = "";
    let quote = "";
    let started = false;
    for (let index = 0; index < line.length; index++) {
      const ch = line[index];
      if (ch === "\\") {
        started = true;
        const escaped = line[++index];
        if (escaped === undefined)
          throw new Error(`第 ${lineIndex + 1} 行：缺少转义字符`);
        const escapes: Record<string, string> = {
          n: "\n",
          r: "\r",
          t: "\t",
          "0": "\0",
        };
        if (escaped === "x") {
          const hex = line.slice(index + 1, index + 3);
          if (!/^[\da-f]{2}$/i.test(hex))
            throw new Error(`第 ${lineIndex + 1} 行：无效的十六进制转义`);
          value += String.fromCharCode(parseInt(hex, 16));
          index += 2;
        } else value += escapes[escaped] ?? escaped;
      } else if (quote) {
        if (ch === quote) quote = "";
        else value += ch;
      } else if (ch === '"' || ch === "'") {
        quote = ch;
        started = true;
      } else if (/\s/.test(ch)) {
        if (started) {
          parts.push(value);
          value = "";
          started = false;
        }
      } else {
        value += ch;
        started = true;
      }
    }
    if (quote) throw new Error(`第 ${lineIndex + 1} 行：引号未闭合`);
    if (started) parts.push(value);
    if (parts.length) commands.push(parts);
  }
  if (!commands.length) throw new Error("没有可执行命令");
  if (commands.length > 200) throw new Error("每次最多执行 200 条命令");
  return commands;
}

export function displayCommand(parts: string[]): string {
  if (parts[0]?.toUpperCase() === "AUTH")
    return parts.length > 2
      ? `AUTH ${JSON.stringify(parts[1])} ••••••`
      : "AUTH ••••••";
  return parts
    .map((part) => (/^[^\s"'\\]+$/.test(part) ? part : JSON.stringify(part)))
    .join(" ");
}

export function textResponse(value: RespValue, depth = 0): string {
  if (value.type === "null") return "(nil)";
  if (value.type === "array") {
    if (!value.value.length) return "(empty array)";
    return value.value
      .map(
        (child, index) =>
          `${"  ".repeat(depth)}${index + 1}) ${textResponse(child, depth + 1)}`,
      )
      .join("\n");
  }
  return value.type === "integer" ? `(integer) ${value.value}` : value.value;
}
