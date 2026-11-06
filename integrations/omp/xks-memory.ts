/**
 * XKS-Memory for omp: every turn of every omp session is stored in the local XKS-Memory store
 * (harness "omp", separate from Claude Code and every other harness) with its exact time, and
 * each prompt receives the related earlier omp turns as context. The same engine and store as
 * the Claude Code plugin; nothing leaves the machine.
 *
 * Install: copy (or link) this file to ~/.omp/agent/hooks/pre/xks-memory.ts (default profile)
 * or ~/.omp/profiles/<name>/agent/hooks/pre/ (named profile). Needs the xksm binary: $XKSM_BIN,
 * ~/.local/bin/xksm, or xksm on PATH.
 *
 * Events (omp extension API):
 *   before_agent_start  recall for the prompt (xksm context), returned as a persisted message;
 *                       then the prompt is stored (xksm turn --role user)
 *   message_end         assistant text, thinking, tool calls; tool results; each stored with
 *                       the message's own timestamp
 * Environment: XKSM_OMP_DISABLE=1 turns it off; XKSM_STORE, XKSM_RECALL_K, XKSM_CONTEXT_CHARS
 * as for the CLI.
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { homedir } from "node:os";
import { basename } from "node:path";

const HARNESS = "omp";

function xksmBinary(): string | null {
  const candidates = [process.env.XKSM_BIN, `${homedir()}/.local/bin/xksm`];
  for (const c of candidates) if (c && existsSync(c)) return c;
  const which = spawnSync("sh", ["-c", "command -v xksm"], { encoding: "utf8" });
  const p = which.stdout?.trim();
  return p ? p : null;
}

const BIN = process.env.XKSM_OMP_DISABLE === "1" ? null : xksmBinary();

/** session id: the session manager's id, else the session file name, else one per process */
const processSession = `omp-${process.pid}-${Date.now()}`;
function sessionId(ctx: any): string {
  const sm = ctx?.sessionManager;
  try {
    const id = sm?.getSessionId?.();
    if (typeof id === "string" && id) return id;
    const file = sm?.getSessionFile?.();
    if (typeof file === "string" && file) return basename(file).replace(/\.jsonl$/, "");
  } catch {
    /* fall through */
  }
  return processSession;
}

/** store one turn asynchronously (the store serialises concurrent writers) */
function store(role: string, session: string, cwd: string | undefined, text: string, ms?: number) {
  if (!BIN || !text) return;
  const args = ["turn", "--harness", HARNESS, "--session", session, "--role", role];
  if (cwd) args.push("--cwd", cwd);
  if (typeof ms === "number" && Number.isFinite(ms)) args.push("--time-us", String(Math.round(ms * 1000)));
  const child = spawn(BIN, args, { stdio: ["pipe", "ignore", "ignore"] });
  child.on("error", () => {});
  child.stdin.on("error", () => {});
  child.stdin.end(text);
}

function textOf(content: any): string {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  return content
    .filter((b) => b && b.type === "text" && typeof b.text === "string")
    .map((b) => b.text)
    .join("\n");
}

function renderArgs(args: any, prefix = ""): string {
  if (args === null || args === undefined) return "";
  if (typeof args !== "object") return `${prefix || "value"}: ${String(args)}\n`;
  let out = "";
  for (const [k, v] of Object.entries(args)) {
    const path = prefix ? `${prefix}.${k}` : k;
    if (v !== null && typeof v === "object") out += renderArgs(v, path);
    else if (v !== null && v !== undefined && v !== "") out += `${path}: ${String(v)}\n`;
  }
  return out;
}

export default function xksMemory(pi: any) {
  if (!BIN) return;

  pi.on("before_agent_start", (event: any, ctx: any) => {
    const prompt: string = typeof event?.prompt === "string" ? event.prompt : "";
    if (!prompt.trim()) return;
    const session = sessionId(ctx);
    const r = spawnSync(BIN, ["context", "--harness", HARNESS, "--session", session], {
      input: prompt,
      encoding: "utf8",
      timeout: 20000,
    });
    store("user", session, ctx?.cwd, prompt, Date.now());
    const context = r.status === 0 ? (r.stdout || "").trim() : "";
    if (!context) return;
    return {
      message: {
        customType: "xks-memory",
        content: context,
        display: false,
        details: { harness: HARNESS, session },
      },
    };
  });

  pi.on("message_end", (event: any, ctx: any) => {
    const m = event?.message;
    if (!m || typeof m !== "object") return;
    const session = sessionId(ctx);
    const ms = typeof m.timestamp === "number" ? m.timestamp : Date.now();
    if (m.role === "assistant" && Array.isArray(m.content)) {
      for (const b of m.content) {
        if (b?.type === "text" && b.text) store("assistant", session, ctx?.cwd, b.text, ms);
        else if (b?.type === "thinking" && b.thinking) store("thinking", session, ctx?.cwd, b.thinking, ms);
        else if (b?.type === "toolCall" && b.name)
          store("tool_use", session, ctx?.cwd, `tool call: ${b.name}\n${renderArgs(b.arguments)}`, ms);
      }
    } else if (m.role === "toolResult") {
      const t = textOf(m.content);
      if (t) store("tool_result", session, ctx?.cwd, (m.isError ? "[error] " : "") + t, ms);
    }
    /* user messages: stored at before_agent_start with the exact prompt */
  });
}
