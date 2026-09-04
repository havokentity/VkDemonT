# Parallel-agent coordination

When multiple Claude Code agents run concurrently in worktrees on the
same host machine, they share the GPU. Without coordination, simultaneous
`demont` smoke runs can saturate the GPU and freeze the desktop.

## Smoke-test serialization

Before invoking the `demont` binary (interactive OR `--smoke-frames`),
acquire an atomic lock via `mkdir`. `mkdir` on a lockdir is
POSIX-guaranteed atomic (works from Git Bash on Windows too), needs no
install, and is released on shell exit via `trap`.

```bash
# Acquire (poll until directory creation succeeds = atomic mutex)
LOCK="$TEMP/demont_test.lockdir"
while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done
trap 'rmdir "$LOCK"' EXIT

# Diagnostic logging (optional but recommended)
echo "$(date -Iseconds) <branch> acquired lock" >> "$TEMP/demont_test_log.txt"

# Run demont (native Vulkan backend on the NVIDIA RTX box)
build/win-clang-release/src/app/demont.exe \
    --smoke-frames=N --r-backend=vulkan \
    --smoke-exec=PATH/TO/fixture.cfg \
    --smoke-capture-out=captures/your_capture.png

echo "$(date -Iseconds) <branch> released lock" >> "$TEMP/demont_test_log.txt"
# trap fires on shell exit and releases the lock
```

Concurrent BUILDS (`cmake --build`) are fine — only serialize the
`demont` invocation itself. The poll interval (0.5 s) is small
relative to typical smoke-test duration (1-5 s) so the contention
window is brief.

## Platform

This fork is **Windows / Vulkan / NVIDIA RTX only** — the macOS/Metal
backend was stripped (see [`HANDOFF.md`](HANDOFF.md)). Build with the
`win-clang-release` (or `win-release` / `win-debug`) preset; the default
backend is native Vulkan. GPU pixel-correctness on real NVIDIA hardware
is the validation step that matters here — agents landing Vulkan-only
changes they can't visually verify should say so in the PR body ("needs
PC validation") rather than claim a look they didn't take.

## File ownership in parallel batches

When multiple agents are working a single batch of issues, each issue
body lists "files OWNED" and "files NOT touched." Strict adherence
prevents merge conflicts. If you find you need a file outside your
ownership, STOP and document it in the PR body rather than touch it
— the orchestrator will mediate.

## Integration branch

The orchestrator maintains `integration/parallel-batch-N` as a
periodically-rebased branch containing every in-flight feature, for
user testing. Feature PRs target `main`; the orchestrator merges
your branch into the integration branch automatically as you push.
You don't need to interact with it.
