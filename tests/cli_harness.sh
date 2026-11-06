#!/bin/sh
# cli_harness.sh: end to end test of the agent-harness commands of the xksm CLI on a scratch
# store: Claude Code hook events (UserPromptSubmit, Stop) with a transcript, every turn kind
# stored once in the right namespace with exact times, recall skipping the current session,
# harness segregation, the generic turn/context commands, pause, forget, and the MCP server.
# usage: cli_harness.sh XKSM_BINARY SCRATCH_DIR
set -u
X=$1
D=$2
mkdir -p "$D"
XKSM_STORE=$D/cli_harness.xksm
XKSM_HOME=$D/cli_home
export XKSM_HOME
export XKSM_STORE
find "$D" -maxdepth 1 -name "cli_harness.xksm*" -type f -delete
[ -d "$D/cli_home" ] && find "$D/cli_home" -depth -delete
fails=0
checks=0
check() { # check NAME EXPECTED ACTUAL
    checks=$((checks + 1))
    if [ "$2" != "$3" ]; then
        fails=$((fails + 1))
        echo "FAIL: $1: expected [$2], got [$3]"
    fi
}
q() { sqlite3 "$XKSM_STORE" "$1"; }

T=$D/cli_s1.jsonl
cat > "$T" <<'EOF'
{"type":"user","sessionId":"sess-one","timestamp":"2026-11-05T14:02:03.250Z","message":{"role":"user","content":"alpha placeholder question about lattice cache eviction"}}
{"type":"assistant","sessionId":"sess-one","timestamp":"2026-11-05T14:02:05.500Z","message":{"role":"assistant","content":[{"type":"thinking","thinking":"placeholder reasoning beta"},{"type":"tool_use","id":"t1","name":"Read","input":{"file_path":"/tmp/placeholder.c","limit":40}}]}}
{"type":"user","sessionId":"sess-one","timestamp":"2026-11-05T14:02:06.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"placeholder file body gamma"}]}}
{"type":"user","sessionId":"sess-one","timestamp":"2026-11-05T14:02:06.100Z","isMeta":true,"message":{"role":"user","content":"meta line never stored"}}
{"type":"user","sessionId":"sess-one","timestamp":"2026-11-05T14:02:06.200Z","message":{"role":"user","content":"<system-reminder>plumbing never stored</system-reminder>"}}
{"type":"assistant","sessionId":"sess-one","timestamp":"2026-11-05T14:02:09.750Z","message":{"role":"assistant","content":[{"type":"text","text":"placeholder answer: the lattice cache eviction uses CLOCK"}]}}
EOF

printf '{"session_id":"sess-one","transcript_path":"%s","cwd":"/tmp/p1","hook_event_name":"UserPromptSubmit","prompt":"alpha placeholder question about lattice cache eviction"}' "$T" | "$X" hook > /dev/null
check "hook exit" 0 $?
printf '{"session_id":"sess-one","transcript_path":"%s","cwd":"/tmp/p1","hook_event_name":"Stop","stop_hook_active":false,"last_assistant_message":"placeholder answer: the lattice cache eviction uses CLOCK"}' "$T" | "$X" hook > /dev/null
check "dialogue memories" 2 "$(q "SELECT count(*) FROM memory WHERE namespace = 'claude-code'")"
check "trace memories" 3 "$(q "SELECT count(*) FROM memory WHERE namespace = 'claude-code-trace'")"
check "reply stored once" 1 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%placeholder answer%'")"
check "meta and plumbing skipped" 0 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%never stored%'")"
check "exact event time" 1793887329750000 "$(q "SELECT event_us FROM memory WHERE text LIKE '%placeholder answer%'")"
check "tool call rendered" 1 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%tool call: Read%file_path: /tmp/placeholder.c%'")"

# readable session files: one folder per session, session.md, one file per stored turn
sd=$(find "$XKSM_HOME/claude-code" -mindepth 1 -maxdepth 1 -type d -name "*_sess-one" 2>/dev/null | head -n 1)
check "session folder" 1 "$( [ -n "$sd" ] && [ -f "$sd/session.md" ] && echo 1 || echo 0)"
check "one file per turn" "$(q "SELECT count(*) FROM source WHERE uri LIKE 'xksm-turn://claude-code/sess-one/%'")" "$(find "$sd" -name "*_*_*.md" ! -name session.md | wc -l | tr -d ' ')"
check "reply file holds the text" 1 "$(grep -l 'placeholder answer: the lattice cache eviction uses CLOCK' "$sd"/*_assistant_*.md | wc -l | tr -d ' ')"
check "turn file time and role in its name" 1 "$(ls "$sd" | grep -c -E '^2026-11-05_08-02-09\.750_assistant_[0-9a-f]{8}\.md$')"
check "file mode 600" 600 "$(stat -c %a "$(ls "$sd"/*_user_*.md | head -n 1)")"
check "folder mode 700" 700 "$(stat -c %a "$sd")"
f=$(ls "$sd"/*_tool_use_*.md | head -n 1)
find "$f" -delete
"$X" mirror > /dev/null
check "mirror restores a file" 1 "$( [ -f "$f" ] && echo 1 || echo 0)"

# a second session: recall finds session one, never the current session
out=$(printf '{"session_id":"sess-two","transcript_path":"%s/none.jsonl","cwd":"/tmp/p2","hook_event_name":"UserPromptSubmit","prompt":"how does cache eviction work in the lattice"}' "$D" | "$X" hook)
check "context injected" 1 "$(printf '%s' "$out" | grep -c 'additionalContext.*session sess-one')"
out=$(printf '{"session_id":"sess-two","transcript_path":"%s/none.jsonl","cwd":"/tmp/p2","hook_event_name":"UserPromptSubmit","prompt":"how does cache eviction work in the lattice"}' "$D" | "$X" hook)
check "own session not recalled" 0 "$(printf '%s' "$out" | grep -c 'session sess-two')"
check "repeated prompt stored twice" 2 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%session sess-two%cache eviction work%'")"

# generic commands, another harness: segregated
printf 'omp placeholder turn about lattice cache eviction' | "$X" turn --harness omp --session o1 --role user --cwd /tmp/o
check "turn exit" 0 $?
check "omp namespace" 1 "$(q "SELECT count(*) FROM memory WHERE namespace = 'omp'")"
check "claude-code context has no omp" 0 "$(printf 'lattice cache eviction' | "$X" context --harness claude-code | grep -c 'omp placeholder')"
check "omp context has no claude-code" 0 "$(printf 'lattice cache eviction' | "$X" context --harness omp --session o2 | grep -c 'placeholder answer')"
check "omp context finds omp" 1 "$(printf 'lattice cache eviction' | "$X" context --harness omp --session o2 | grep -c 'omp placeholder')"
check "bad harness refused" 1 "$(printf 'x' | "$X" turn --harness 'Bad/Name' --session s --role user 2>/dev/null; echo $?)"
check "sessions listed" 3 "$("$X" sessions | grep -c -E 'sess-one|sess-two|o1')"

# PostToolUse during a turn: the tool call and its result are stored before the turn ends
T3=$D/cli_s3.jsonl
cat > "$T3" <<'EOT'
{"type":"user","sessionId":"sess-three","timestamp":"2026-11-05T15:00:00.000Z","message":{"role":"user","content":"placeholder question iota"}}
{"type":"assistant","sessionId":"sess-three","timestamp":"2026-11-05T15:00:01.000Z","message":{"role":"assistant","content":[{"type":"tool_use","id":"t3","name":"Bash","input":{"command":"placeholder kappa"}}]}}
{"type":"user","sessionId":"sess-three","timestamp":"2026-11-05T15:00:02.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t3","content":"placeholder output lambda"}]}}
EOT
printf '{"session_id":"sess-three","transcript_path":"%s","cwd":"/tmp/p3","hook_event_name":"PostToolUse","tool_name":"Bash"}' "$T3" | "$X" hook > /dev/null
check "post-tool-use exit" 0 $?
check "tool call stored mid-turn" 1 "$(q "SELECT count(*) FROM memory WHERE namespace = 'claude-code-trace' AND text LIKE '%session sess-thr%placeholder kappa%'")"
check "tool result stored mid-turn" 1 "$(q "SELECT count(*) FROM memory WHERE namespace = 'claude-code-trace' AND text LIKE '%session sess-thr%placeholder output lambda%'")"
printf '{"session_id":"sess-three","transcript_path":"%s","cwd":"/tmp/p3","hook_event_name":"PostToolUse","tool_name":"Bash"}' "$T3" | "$X" hook > /dev/null
check "a second PostToolUse stores nothing twice" 2 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%session sess-thr%' AND (text LIKE '%kappa%' OR text LIKE '%lambda%')")"
check "mid-turn session file" 1 "$(find "$XKSM_HOME/claude-code" -path '*_sess-three/*_tool_result_*.md' | wc -l | tr -d ' ')"

# MCP over stdio
mcp=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"0"}}}' \
    '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
    '{"jsonrpc":"2.0","id":"c3","method":"tools/call","params":{"name":"recall","arguments":{"query":"cache eviction","limit":2}}}' \
    '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"search","arguments":{"query":"Read","scope":"all"}}}' \
    '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"remember","arguments":{"text":"note: placeholder decision delta"}}}' \
    '{"jsonrpc":"2.0","id":7,"method":"bogus"}' \
    'not json' | "$X" mcp)
check "mcp replies" 7 "$(printf '%s\n' "$mcp" | grep -c '"jsonrpc":"2.0"')"
check "mcp initialize" 1 "$(printf '%s\n' "$mcp" | grep -c '"id":1,"result":{"protocolVersion":"2025-06-18"')"
check "mcp tools" 1 "$(printf '%s\n' "$mcp" | grep -c '"name":"recall".*"name":"search".*"name":"remember".*"name":"sessions".*"name":"stats"')"
check "mcp recall" 1 "$(printf '%s\n' "$mcp" | grep -c '"id":"c3".*session sess-one')"
check "mcp search trace" 1 "$(printf '%s\n' "$mcp" | grep -c '"id":4.*tool call: Read')"
check "mcp remember" 1 "$(printf '%s\n' "$mcp" | grep -c '"id":5.*stored at 20')"
check "mcp unknown method" 1 "$(printf '%s\n' "$mcp" | grep -c '"id":7,"error":{"code":-32601')"
check "mcp parse error" 1 "$(printf '%s\n' "$mcp" | grep -c '"id":null,"error":{"code":-32700')"

# pause and forget
"$X" pause claude-code on > /dev/null
printf '{"session_id":"sess-two","hook_event_name":"UserPromptSubmit","prompt":"stored while paused"}' | "$X" hook > /dev/null
check "paused stores nothing" 0 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%stored while paused%'")"
"$X" pause claude-code off > /dev/null
"$X" forget claude-code sess-one > /dev/null
check "forget memories" 0 "$(q "SELECT count(*) FROM memory WHERE text LIKE '%session sess-one%'")"
check "forget sources" 0 "$(q "SELECT count(*) FROM source WHERE uri LIKE 'xksm-turn://claude-code/sess-one/%'")"
check "forget fts" 0 "$(q "SELECT count(*) FROM memory_fts WHERE memory_fts MATCH 'CLOCK'")"
check "other harness kept" 1 "$(q "SELECT count(*) FROM memory WHERE namespace = 'omp'")"
check "forget removed the session folder" 0 "$(find "$XKSM_HOME/claude-code" -mindepth 1 -maxdepth 1 -name "*_sess-one" | wc -l | tr -d ' ')"
check "other harness files kept" 1 "$(find "$XKSM_HOME/omp" -name "*_user_*.md" | wc -l | tr -d ' ')"

# unreadable hook input never fails the session
check "garbage hook input exit 0" 0 "$(printf 'garbage' | "$X" hook 2>/dev/null; echo $?)"

echo "cli_harness: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ]
