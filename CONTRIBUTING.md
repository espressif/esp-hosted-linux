# Contributing

Contributions are submitted through GitLab merge requests.

## Development workflow

1. Create a topic branch from the current `master`.
2. Keep commits focused and logically separated.
3. Sign off every commit you author:

       git commit -s

4. Rebase the topic branch when necessary instead of merging `master` into it.
   Merge commits inside a merge request are not accepted.
5. Open a merge request and describe the problem, implementation, related issue
   when applicable, compatibility impact, and validation performed.
6. Complete the **Release notes** section in the merge request description.
   Release-note information is kept in GitLab and is not added to commit
   messages solely for release-note tracking.
7. Address review feedback and keep all required CI checks green.

## Merge requirements

A merge request can be merged only after the required CI pipeline succeeds and
the required Maintainer approval is recorded in GitLab.

The default branch uses linear history. Merge requests are integrated using
fast-forward-only merging and GitLab does not squash the commits.

## Commit sign-off

Every commit must contain the `Signed-off-by:` trailer of its author.

`Signed-off-by:` certifies the contribution made by the commit author. It is
not a review or approval marker. Maintainer review and approval are recorded
by GitLab.

Do not add another contributor or reviewer as `Signed-off-by:` unless that
person explicitly provided that certification.

## Reporting issues

Include enough information to reproduce and diagnose the problem when
applicable: platform, kernel or ESP-IDF version, transport, hardware target,
logs, packet captures, and reproduction steps.
