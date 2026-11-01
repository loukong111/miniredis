import { describe, expect, it } from "vitest";
import { RespDecoder, encodeCommand } from "../src/main/protocol";
import {
  displayCommand,
  parseScript,
  textResponse,
} from "../src/shared/commands";

describe("RESP framing", () => {
  it("decodes split UTF-8 and keeps identical pipelined responses", () => {
    const bytes = Buffer.from(
      "$6\r\n你好\r\n+OK\r\n+OK\r\n:9223372036854775807\r\n",
    );
    const decoder = new RespDecoder();
    const values = [...bytes].flatMap((byte) =>
      decoder.feed(Buffer.from([byte])),
    );
    expect(values).toEqual([
      { type: "bulk", value: "你好" },
      { type: "simple", value: "OK" },
      { type: "simple", value: "OK" },
      { type: "integer", value: "9223372036854775807" },
    ]);
  });
  it("uses byte lengths, nested arrays and nulls", () => {
    expect(new RespDecoder().feed(encodeCommand(["SET", "键", ""]))).toEqual([
      {
        type: "array",
        value: [
          { type: "bulk", value: "SET" },
          { type: "bulk", value: "键" },
          { type: "bulk", value: "" },
        ],
      },
    ]);
    expect(
      new RespDecoder().feed(Buffer.from("*3\r\n$-1\r\n*0\r\n*1\r\n:2\r\n"))[0],
    ).toEqual({
      type: "array",
      value: [
        { type: "null" },
        { type: "array", value: [] },
        { type: "array", value: [{ type: "integer", value: "2" }] },
      ],
    });
  });
  it.each([
    "$-2\r\n",
    "$1\r\nxZZ",
    "*100001\r\n",
    "+".repeat(5000),
    "?bad\r\n",
    "*1\r\n".repeat(66),
  ])("rejects invalid or excessive input", (wire) => {
    expect(() => new RespDecoder().feed(Buffer.from(wire))).toThrow();
  });
});

describe("command scripts", () => {
  it("handles empty quoted arguments, comments and escapes", () => {
    expect(
      parseScript('# demo\nSET "a b" ""\nSET unicode "你好\\nnext\\x21"'),
    ).toEqual([
      ["SET", "a b", ""],
      ["SET", "unicode", "你好\nnext!"],
    ]);
  });
  it("validates the whole script before executing anything", () => {
    expect(() => parseScript('SET valid yes\nSET broken "no')).toThrow(
      "第 2 行",
    );
    expect(() => parseScript("PING\n".repeat(201))).toThrow("200");
    expect(() => parseScript('SET a "\\xzz"')).toThrow("十六进制");
  });
  it("redacts both AUTH forms and formats nested results", () => {
    expect(displayCommand(["auth", "never-display-this"])).toBe("AUTH ••••••");
    expect(displayCommand(["AUTH", "alice", "never-display-this"])).toBe(
      'AUTH "alice" ••••••',
    );
    expect(
      textResponse({
        type: "array",
        value: [{ type: "null" }, { type: "integer", value: "2" }],
      }),
    ).toBe("1) (nil)\n2) (integer) 2");
  });
});
