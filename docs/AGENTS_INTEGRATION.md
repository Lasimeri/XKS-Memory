# Agent harness integration

How XKS-Memory records and recalls the turns of AI agent harnesses: Claude Code, omp, and any
other program that can run a command. Source: `cli/harness.c`, `src/core/session.c`,
`integrations/`.

## 1. Model

- **Harness:** a named agent program, `[a-z0-9._-]{1,48}`: `claude-code`, `omp`, or any name
  you choose for another tool.
- **Session:** one conversation of a harness, identified by the harness's own session id.
- **Turn:** one stored piece of a session. It is one SQLite `source` row with uri
  `xksm-turn://HARNESS/SESSION/ROLE/TIME_US`, plus one memory per chunk (about 1 KiB, split at
  paragraphs). Every chunk carries the turn's time in `event_us` (microseconds).
- **Roles:** `user`, `assistant`, `note` are stored in the namespace `HARNESS` (dialogue);
  `tool_use`, `tool_result`, `thinking` are stored in `HARNESS-trace`. Recall into a prompt
  searches only the dialogue namespace of the same harness, so harnesses never mix.
- **Header:** the first line of each turn's text is
  `2026-11-06T10:35:03.123-06:00 | role | session 1a2b3c4d`: exact local time with UTC offset,
  readable in results and searchable as words.
- **Registry:** table `session(harness, id, first_us, last_us, turns, cwd)`, updated on every
  stored turn.

## 2. Storage location and privacy

| Item | Path |
|---|---|
| Store | `$XKSM_STORE`, else `$XDG_DATA_HOME/xksm/agents.xksm`, else `~/.local/share/xksm/agents.xksm` |
| Snapshots | `agents.xksm.vidx-<hash>` (vector index) and `agents.xksm.ptgt` (port targets), beside the store, rebuilt automatically |
| Application | `~/.local/share/xks-memory/` (installed files only, never data) |

The CLI sets `umask 077`: the store is created mode 600 in a mode-700 directory. Deleting a
memory or forgetting a session removes the snapshot files at once, so no deleted vector
remains on disk.

## 3. Claude Code

Plugin: `integrations/claude-code/plugin` (marketplace at `integrations/claude-code`). Install:

```
claude plugin marketplace add ~/.local/share/xks-memory/claude-code
claude plugin install xks-memory@xks-memory
```

| Hook | What `xksm hook` does |
|---|---|
| UserPromptSubmit (timeout 20 s) | Imports the transcript lines written since the last call (bounded at 8 MiB per call, `XKSM_INGEST_BUDGET`). Recalls the best earlier turns of other sessions for the prompt and returns them as `hookSpecificOutput.additionalContext` (`XKSM_RECALL_K` hits, default 4, at most `XKSM_CONTEXT_CHARS`, default 4000, capped at 9500 below Claude Code's 10,000-character limit). Stores the prompt and embeds it. |
| Stop (async) | Imports the rest of the transcript; stores `last_assistant_message` unless the transcript already held it; keeps the word-vector table current. |
| PreCompact, SessionEnd | Import the transcript tail. |

The transcript import stores every user and assistant text block, every tool call (name and
rendered input), every tool result (up to `XKSM_TRACE_MAX` bytes per block, default 65,536,
with a note when cut) and every thinking block. It skips harness plumbing (system reminders,
slash-command echoes, meta lines, sidechains). The byte offset per transcript is kept in the
store, so every line is read once. A dialogue text that was stored from the hook input and
appears again in the transcript is stored once; an identical prompt typed twice is stored
twice.

MCP server (`xksm mcp --harness claude-code`, declared in the plugin's `.mcp.json`):

| Tool | Arguments | Result |
|---|---|---|
| `recall` | `query`, `limit` (8), `scope`: `dialogue`, `all` (+ trace), `all-harnesses` | earlier turns by words and meaning |
| `search` | `query`, `limit`, `scope` | exact word search (BM25) |
| `remember` | `text` | stores a note with the current time |
| `sessions` | `limit` | recorded sessions with turns and time ranges |
| `stats` | | store size and state |

Backfill: `xksm transcripts` imports every top-level transcript under `~/.claude/projects`
(or the files named). Measured: 66 transcripts, 1.35 GB, 320,503 lines in 35.96 s.

## 4. omp

`integrations/omp/xks-memory.ts`, an omp extension, and `integrations/omp/mcp.json`, the MCP
server definition (`xksm mcp --harness omp`; copy it to `~/.omp/agent/mcp.json` or
`~/.omp/profiles/NAME/agent/mcp.json`; the omp-managed file wins over the `xks-memory` server
omp would otherwise import from the Claude Code plugin, which uses `--harness claude-code`).
Link the extension into a profile's hook directory:
`~/.omp/agent/hooks/pre/` (default profile) or `~/.omp/profiles/NAME/agent/hooks/pre/`.

| Event | Action |
|---|---|
| `before_agent_start` | `xksm context --harness omp` for the prompt, returned as a persisted message (customType `xks-memory`); then the prompt is stored |
| `message_end` | assistant text, thinking and tool calls of each assistant message, and each tool result, stored with the message's own timestamp |

The binary is found through `$XKSM_BIN`, `~/.local/bin/xksm` or `PATH`;
`XKSM_OMP_DISABLE=1` turns the extension off.

## 5. Any other harness

```
printf '%s' "$text"   | xksm turn --harness NAME --session ID --role ROLE [--cwd DIR] [--time-us T]
printf '%s' "$prompt" | xksm context --harness NAME [--session ID]
```

`turn` stores and embeds; with `--role assistant` it also rebuilds the word-vector table when
the store has grown 25 % since the last build. `context` prints the recalled turns (nothing
when there are none); a session given with `--session` is excluded, because its turns are
already in the conversation.

## 6. Management

| Command | Effect |
|---|---|
| `xksm sessions [HARNESS]` | sessions, most recent first |
| `xksm forget HARNESS SESSION` | deletes every memory and source of the session (secure delete), its row and its snapshots |
| `xksm pause HARNESS on|off` | stops or resumes logging (the hook still answers and stores nothing); `XKSM_PAUSE=1` pauses every harness for one process |
| `xksm-tui` | interactive view of all of the above (see the README) |

## 7. Embedding upkeep

- New memories are embedded with the current word-vector table at once (fold-in). Only
  memories above a watermark are examined, so the cost does not grow with the store.
- The table is rebuilt in an async Stop hook when it does not exist and the store has at
  least 200 memories, or when the store has grown 25 % since the build. A second process
  does not start a build while one is running (120 s guard).
- Words that are new since the last build carry no vector signal until the next build; BM25
  finds them at once.

## 8. Measured cost

On a 150,951-memory store (Ryzen 7 5800X), UserPromptSubmit with recall, store and embed
takes 0.17 to 0.18 s (3 runs). Its phases:

| Phase | Time |
|---|---|
| Recall | 0.134 to 0.145 s, of which BM25 0.039 s, word table load about 0.06 s, vector search 0.035 s cold |
| Embed (fold-in) | 0.032 s |

Stop takes 0.13 s. `XKSM_PROFILE=1` prints the phase times of a hook call on stderr.

## 9. Threads

Two pools per process, 4 workers each: recall (vector scans, port expansion) and embedding
(table build). BM25 runs in its own thread on a read-only second connection while the vector
branch runs. `XKSM_THREADS` and `XKSM_EMBED_THREADS` (1..128) change the pool sizes;
`XKSM_SERIAL_RECALL=1` runs the recall branches one after the other.
