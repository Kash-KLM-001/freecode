# freecode

A tiny OpenCode-style terminal UI in C11 with a synchronized multi-model team.

The default backend is the **Kilo AI Gateway anonymous free tier**: no account,
no API key, and no paid models. A startup selector fetches the live catalog,
keeps only free/no-auth models, ranks them for coding, and builds the default
team from the winners.

The default build has **no linked third-party dependencies**: raw ANSI escapes,
`termios`, `poll()`, POSIX sockets, and the system `curl` command for HTTPS.
An optional `make libcurl` build is provided if you prefer linking libcurl.

## Build

```sh
make
./freecode
```

Optional linked-libcurl build:

```sh
make libcurl
```

Clean:

```sh
make clean
```

## glibc and bionic

The same source builds against **glibc** (Linux) and **bionic** (Android /
Termux). The default build has no linked third-party dependencies, and the
POSIX feature macros in the Makefile are honored by both libcs.

```sh
make            # host build: glibc on Linux, bionic on Android/Termux
make bionic     # freecode-bionic, kmodels-bionic (BIONIC_CC overrides compiler)
make glibc      # freecode-glibc, kmodels-glibc
make both       # both at once
./scripts/build-libcs.sh   # build both and show each ELF interpreter
```

`make glibc` uses the native glibc compiler when the host already has one. On
a bionic host it cross-compiles with clang and the bundled glibc sysroot, by
default `/data/data/com.termux/files/usr/glibc`. Override with
`GLIBC_SYSROOT=/path`, `GLIBC_CC=clang`, `GLIBC_TRIPLE=aarch64-unknown-linux-gnu`,
`GLIBC_CRT_DIR=/path/lib`, or `GLIBC_LD=lld` if your environment differs.

`make bionic` uses `$(CC)` when it already targets Android. On a glibc host,
pass an NDK clang wrapper, e.g.
`make bionic BIONIC_CC=aarch64-linux-android24-clang`.

The glibc build links `libm`; the bionic build does not need it. Neither build
requires a system-wide libcurl (that remains the optional `make libcurl`
target).

## Free model selector

`scripts/select-free-model.sh` checks the live Kilo catalog and prints only
models that are free and usable anonymously. It ranks coding-oriented models
higher and prefers larger contexts and non-training providers.

```sh
./scripts/select-free-model.sh                # best model ID
./scripts/select-free-model.sh --list 5       # top 5 free anonymous IDs
./scripts/select-free-model.sh --json         # ranked JSON
./scripts/select-free-model.sh --probe        # verify with a tiny chat request
./scripts/select-free-model.sh --catalog      # inspect full catalog flags
```

`kmodels` is the compiled selector used by the script. The app uses the same
catalog/selection code at startup, so the script and the UI agree.

## Keys and workflow

| Key | Action |
|---|---|
| `Enter` | Send to the team (team mode) or local stub (local mode) |
| `Ctrl+P` | Command palette |
| `Ctrl+T` | Toggle team / local view |
| `Ctrl+B` | Toggle broadcast vs focused slot |
| `Ctrl+N` | New local session |
| `Up` / `Down` | Scroll transcript |
| `PgUp` / `PgDn` | Scroll by 10 lines |
| `Ctrl+U` | Clear prompt |
| `Ctrl+L` | Force redraw |
| `Ctrl+C` / `Ctrl+D` | Quit |

## Team mode

The sidebar lists every model slot with:

```text
> planner/plan idle 7/7 kilo-auto/free
  reviewer/review idle 7/7 reflection/beam-free
```

`seen/total` is the synchronization cursor. Every slot reads the same
append-only team log. In broadcast mode, a round is serialized: slot 0 answers,
then slot 1, and so on, so every model sees earlier replies. In focus mode only
the focused slot answers, but all slots still advance their sync cursor.

### Team / model commands

| Command | Action |
|---|---|
| `/team` | Team view |
| `/local` | Local session view |
| `/local URL MODEL` | Configure a local OpenAI-compatible route |
| `/add NAME ROLE MODEL` | Add a model slot |
| `/rm NAME` | Remove a model slot |
| `/focus NAME` | Focus one slot and disable broadcast |
| `/sync` | Toggle broadcast/focus |
| `/broadcast` | Broadcast to all slots |
| `/models` | List free candidates and slot models |
| `/discover` | Re-fetch Kilo free models and rebuild candidates |
| `/model ID` | Change the focused slot's model |
| `/smodel NAME MODEL` | Change a named slot's model |
| `/provider NAME` | `kilo`, `local`, `openai`, `anthropic`, or `custom` |
| `/route URL` | Set base URL / custom route |
| `/apikey KEY` | Set API key (masked in the log) |
| `/usage` | Show token/cost/rate usage |
| `/compact [reason]` | Force compaction |
| `/rotate` | Move every slot to the next free candidate |
| `/threshold PCT` | Set compaction threshold |
| `/stub` | Toggle the offline streaming stub |
| `/save` | Save config with mode `0600` |
| `/memory` | Show memory file and restored message counts |
| `/clearmemory` / `/forget` | Wipe saved memory and start a fresh transcript |
| `/teamhelp` | Print all commands in the log |

Local session commands still work: `/new`, `/next`, `/clear`, `/redraw`,
`/help`, `/quit`.

### Custom routes and API keys

Examples:

```text
/local http://127.0.0.1:11434/v1 qwen2.5-coder:7b
/provider openai
/route https://api.openai.com/v1
/apikey sk-...
/save
```

Kilo free usage needs none of this: omitting the key sends no `Authorization`
header at all. Explicit API keys are stored only in
`$XDG_CONFIG_HOME/freecode/config` (or `~/.config/freecode/config`) with mode
`0600`. A non-secret template is provided as `config.example`.

Environment overrides: `FREECODE_PROVIDER`, `FREECODE_BASE_URL`,
`FREECODE_API_KEY`, `FREECODE_MODEL`, `FREECODE_FREE_ONLY`, `FREECODE_STUB`,
`FREECODE_WARN_PCT`, `FREECODE_COMPACT_PCT`, `FREECODE_TEAM_SIZE`,
`FREECODE_TRANSPORT`, `FREECODE_MEMORY_FILE`, plus the usual provider key
variables.

`FREECODE_NO_DISCOVER=1` skips the startup catalog fetch and uses built-in free
fallbacks. `FREECODE_STUB=1` starts fully offline.

## Automatic chat memory

Chat memory saves itself — you do not need to run a command to keep it.

- Local sessions and the synchronized team log are written to a JSONL file
  under `$XDG_STATE_HOME/freecode/memory.jsonl` (or
  `~/.local/state/freecode/memory.jsonl`), mode `0600`.
- Save happens automatically as messages arrive (throttled to at most once every
  2 seconds) and again on exit, so closing the terminal keeps the conversation.
- On startup the file is loaded before the provider is configured. The full
  transcript, the compaction summary, slot models, focus, and broadcast mode are
  restored. The restored log is fed into the next team request, so the agents
  genuinely remember earlier turns instead of only redisplaying them.
- Override the location with `FREECODE_MEMORY_FILE=/path/to/memory.jsonl`.
- `/memory` reports the path and message counts and forces a flush.
- `/clearmemory` (alias `/forget`) deletes the file and resets the in-memory
  transcript. Auto-save stays on afterwards.

The memory file contains chat text only. API keys are never written to it; they
remain in the separate `0600` config file.

## Compaction and usage

The UI tracks prompt/completion tokens, cost, context usage, and anonymous
request count. When usage crosses the compaction threshold (default `88%`) or
the anonymous rate limit approaches Kilo's free-tier cap, freecode:

1. asks a free model to compress the shared transcript into a dense summary,
2. replaces the log with that summary plus the newest messages,
3. rotates every slot to the next free model, and
4. feeds the summary into the next team request.

This mirrors the useful part of OpenCode-style compaction without carrying the
full frontend stack. Use `/threshold`, `/compact`, and `/rotate` to control it.

## Transports

- `http://` routes use POSIX sockets directly.
- `https://` routes use the system `curl` subprocess by default (no linked
  dependency).
- `make libcurl` links libcurl for synchronous requests.
- Set `FREECODE_TRANSPORT=auto|curl|direct|libcurl` to force one.

## What is implemented

- Differential cell-buffer renderer with one buffered write per frame
- UTF-8 input and `wcwidth()` layout
- Sessions, command palette, slash commands, scrolling, resizing
- Kilo free/no-auth discovery with ranked model selection
- Synchronized multi-model team with serialized rounds
- OpenAI-compatible chat streaming and Anthropic native messages
- Local models, custom routes, provider API keys
- Token/cost/rate usage tracking, auto-compaction, model rotation
- Automatic persistent chat memory with restored agent context
- Portable glibc and bionic builds with explicit `make` targets

## Scope

No tools, file search, diffs, plugins, or provider accounts are required for the
default free workflow. Paid models are never selected automatically; they are
only reachable when you explicitly configure a route/key and model.

Repository: <https://github.com/Kash-KLM-001/freecode>
