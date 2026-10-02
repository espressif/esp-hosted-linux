# Contributing

Contributions are welcome through GitHub issues and pull requests.

## Development workflow

1. Fork the repository, or create a branch directly if you have write access.
2. Create a topic branch from `master`.
3. Keep commits focused and reviewable.
4. Sign off every commit you author:

   ```sh
   git commit -s
   ```

5. Run the checks and tests that cover your change.
6. Push your branch and open a GitHub pull request against `master`.
7. Explain the problem, what changed, any compatibility impact, and how you tested it.
8. Address review feedback and keep required checks passing.

For a larger behavior, API, or compatibility change, opening a GitHub issue first makes it easier to agree on the direction before implementation.

## Local checks

Run the repository pre-commit hooks before pushing:

```sh
pre-commit run --all-files
```

For driver or firmware changes, run the build/test flow that covers the code you changed and include the result in the pull request.

## Documentation changes

Edit Markdown under `docs/` and the top-level `README.md` when needed. Do not edit `docs-html/` by hand.

Project automation rebuilds the static HTML documentation when documentation source changes. It also verifies that committed `docs-html/` matches a fresh strict build, so stale or hand-edited generated files cannot pass validation.

## Commit sign-off

Every authored commit must contain its author's `Signed-off-by:` trailer. `git commit -s` adds it automatically.

Sign-off certifies the contribution from that author. It is not a review or approval marker. Do not add another person's sign-off unless that person explicitly provided it.

## Pull request review

Maintainers review pull requests before merge. Keep the branch up to date when requested and resolve review comments before the change is merged.

## Reporting issues

Open a GitHub issue and include enough information to reproduce the problem:

- ESP target and board/module
- Linux host and kernel
- SDIO/SPI/UART setup and bus clock or UART baud rate
- ESP-IDF/firmware and host-driver revision
- host `dmesg`
- ESP serial log
- relevant `wpa_supplicant`, `hostapd`, or BlueZ logs
- packet capture when the failure is protocol-related
- clear reproduction steps

The [troubleshooting guide](docs/troubleshooting.md) lists useful checks and logs by failure type.
