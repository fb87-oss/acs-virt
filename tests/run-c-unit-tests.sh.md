# run-c-unit-tests.sh

Builds and runs the C backend unit tests in `tests/c-backend-tests.c`.

The script enters a Nix shell with `gcc` when needed, builds
`build/out/c-backend-tests`, and forwards any arguments to the `ctest.h` test runner.
For example, `tests/run-c-unit-tests.sh blkd_block` runs only the block backend
suite.
