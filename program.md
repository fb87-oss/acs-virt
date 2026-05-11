# autoresearch: UIO throughput

This is an autonomous experiment loop for improving `axi-linux-uio` block
throughput in this repository.

## Setup

1. Agree on a run tag.

   Recommended tag for this run: `may11-uio-throughput`.

   The branch `autoresearch/<tag>` must not already exist. This should be a
   fresh run from the current baseline branch.

2. Create the branch.

   ```sh
   git checkout -b autoresearch/may11-uio-throughput
   ```

3. Confirm the working tree is clean before starting experiments.

   ```sh
   git status --short
   ```

4. Read the in-scope context.

   Important files and directories:

   - `README.md`: repository overview and build flow.
   - `docs/project-state-and-design.md`: current topology and address layout.
   - `docs/uio-fabric.md`: UIO topology, benchmark notes, and profiling notes.
   - `tests/run-benchmark.sh`: benchmark entry point.
   - `tests/run-benchmark.md`: benchmark behavior and output format.
   - `scripts/chiplets-uio-x64.py`: two-VM UIO launcher.
   - `scripts/build-qemu-x64.sh`: rebuilds patched x86_64 QEMU.
   - `src/`: backend daemons, virtio helpers, and backend fabrics.
   - `patches/qemu/`: QEMU `axi` device and platform integration patches.

5. Verify the benchmark path works.

   ```sh
   BENCH_SIZE_MB=1 BENCH_REPEAT=1 tests/run-benchmark.sh
   ```

   If this fails, stop and fix the environment or baseline before starting the
   research loop.

6. Initialize `results.tsv` with only the header row.

   ```text
   commit	write_mibs	read_mibs	status	description
   ```

   Do not commit `results.tsv`. It is an experiment log, not source state.

7. Run the baseline benchmark first, before making any experimental changes.

   Recommended baseline command:

   ```sh
   BENCH_SIZE_MB=64 BENCH_REPEAT=3 tests/run-benchmark.sh > run.log 2>&1
   ```

   Record the baseline in `results.tsv` with status `keep` and description
   `baseline`.

## Experiment Scope

The goal is to improve end-to-end x86_64 `axi-linux-uio` virtio-blk throughput.

Primary metric:

- Higher average write MiB/s from the benchmark summary.
- Higher average read MiB/s from the benchmark summary.

Keep criterion:

- Keep a change if it improves average read or write throughput by at least 5%
  and does not regress the other direction by more than 5%.
- Keep a smaller improvement if it substantially simplifies the implementation.
- Discard a change if it is equal, noisier, more complex without clear benefit,
  or regresses one direction significantly.

Editable files:

- `src/`
- `patches/qemu/`
- Documentation for any kept behavior change.
- Build/test scripts only when needed to run or measure the experiment.

Avoid changing:

- Benchmark semantics in `tests/run-benchmark.sh` unless the change is only to
  make measurement more reliable and is recorded clearly.
- The user-visible topology contract without an explicit reason.
- Address layout unless the experiment specifically targets address mapping.

## Benchmark Command

Use this command for normal experiments:

```sh
BENCH_SIZE_MB=64 BENCH_REPEAT=3 tests/run-benchmark.sh > run.log 2>&1
```

Use this faster command only for smoke-checking crashes before a real run:

```sh
BENCH_SIZE_MB=1 BENCH_REPEAT=1 tests/run-benchmark.sh > run.log 2>&1
```

If backend profiling is needed for diagnosis:

```sh
CHIPLETS_PROFILE_BACKEND=1 BENCH_SIZE_MB=64 BENCH_REPEAT=1 tests/run-benchmark.sh > run.log 2>&1
```

If direct read-DMA is part of the experiment:

```sh
CHIPLETS_DIRECT_READ_DMA=1 BENCH_SIZE_MB=64 BENCH_REPEAT=3 tests/run-benchmark.sh > run.log 2>&1
```

## Build Rules

If only `src/` changed, `tests/run-benchmark.sh` rebuilds the backend daemons via
`nix run .#runuio-x64`.

If `patches/qemu/` changed, rebuild x86_64 QEMU before benchmarking:

```sh
scripts/build-qemu-x64.sh
```

Then run the benchmark.

## Output Parsing

Benchmark logs include summary lines like:

```text
write summary: min=18.0MiB/s avg=19.2MiB/s max=20.1MiB/s
read summary:  min=33.7MiB/s avg=34.4MiB/s max=35.0MiB/s
```

Extract the average values from `run.log` and record them in `results.tsv`.

If the summary lines are missing, inspect the end of the log:

```sh
tail -n 80 run.log
```

Treat missing summary lines as a crash unless the failure is clearly unrelated to
the experiment and easy to rerun.

## Logging Results

Append one tab-separated row per experiment to `results.tsv`:

```text
commit	write_mibs	read_mibs	status	description
```

Columns:

1. `commit`: short 7-character git commit hash for the experiment. Use the
   current baseline commit for the baseline row.
2. `write_mibs`: average write MiB/s. Use `0.0` for crashes.
3. `read_mibs`: average read MiB/s. Use `0.0` for crashes.
4. `status`: `keep`, `discard`, or `crash`.
5. `description`: short description of the idea tried.

Example:

```text
commit	write_mibs	read_mibs	status	description
39b88c3	18.0	33.7	keep	baseline
a1b2c3d	19.5	34.0	keep	reduce notify delay for x64 frontend
b2c3d4e	17.8	33.8	discard	batch used-ring updates
c3d4e5f	0.0	0.0	crash	remove notify ack wait
```

Do not commit `results.tsv` or `run.log`.

## Experiment Loop

Loop until manually stopped:

1. Inspect current state.

   ```sh
   git status --short
   git log -1 --oneline
   ```

2. Choose one hypothesis and make the smallest useful change.

3. Run fast static validation if appropriate.

   ```sh
   git diff --check
   tests/run-c-unit-tests.sh
   ```

4. Rebuild QEMU if a QEMU patch changed.

   ```sh
   scripts/build-qemu-x64.sh
   ```

5. Commit the experiment.

   Use a concise imperative commit message describing the idea, for example:

   ```sh
   git add <changed files>
   git commit -m "Tune UIO notify delay"
   ```

6. Run the benchmark.

   ```sh
   BENCH_SIZE_MB=64 BENCH_REPEAT=3 tests/run-benchmark.sh > run.log 2>&1
   ```

7. Parse the result from `run.log`.

8. Record a row in `results.tsv`.

9. Decide keep or discard.

   - If the experiment improves throughput enough, keep the commit and continue
     from it.
   - If the experiment is equal or worse, append a `discard` row, then reset back
     to the previous kept commit.
   - If the experiment crashes and cannot be trivially fixed, append a `crash`
     row, then reset back to the previous kept commit.

10. Continue with a new hypothesis.

## Candidate Hypotheses

Start simple and change one thing per experiment.

- Tune `notify-delay-us` for x86_64 frontend/backend.
- Compare `notify-ack=on` and `notify-ack=off` for x86_64 only.
- Reduce backend IRQ/control socket round trips in the QEMU UIO path.
- Optimize the QEMU `axi` UIO frontend notify path.
- Optimize `linux_uio.c` DMA copy paths.
- Expand or reduce virtio-blk segment size and segment count.
- Batch used-ring updates or interrupt delivery in `virtio-blkd`.
- Re-test direct read-DMA and isolate write-side variance.
- Use backend profiling to target descriptor-chain, guest-DMA, image-I/O,
  used-ring, or IRQ costs.

## Safety Rules

- Never use destructive git commands unless the current experiment has already
  been logged and the reset target is the previous kept commit.
- Never discard untracked `results.tsv` or `run.log` by accident.
- Do not force-push or rewrite shared history.
- Keep experiments small enough that performance changes are attributable.
- Update documentation only for kept behavior changes.
- Prefer simple changes. Complexity must buy measurable throughput.
