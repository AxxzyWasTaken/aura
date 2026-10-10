# Aura licensing and provenance

Aura is an independent Aurora Silicon bootloader. Its initial main branch
continues aurora-silicon/m1n1 aurora-wip at `65e063ceee96492e5668fb1131c3d52343c10098`. The original Git
history and authorship are preserved without rewriting commits.

Aurora contributors' authorized work is published here under
GPL-2.0-only. New project contributions must use GPL-2.0-only and include a
Signed-off-by trailer. Existing upstream code and third-party imports keep
their original licences and notices.

[aurora-contributions.json](../LICENSES/aurora-contributions.json) records the
Aurora contribution commits in the initial migration. The inherited base
revision is `b4654b32941d51afdb77579d63e7cb1aa6c03ecc`. Later inherited upstream commits remain MIT; they are
not included in that Aurora contribution list. The list records commit
provenance, not ownership of every line in a modified file. Inherited code
within such files remains subject to its original notice.

For mixed MIT/Aurora source files, `MIT AND GPL-2.0-only` records both
applicable terms. Newly authored Aurora files use `GPL-2.0-only`. Third-party
license headers and copyright notices must be preserved. See
[MIT](../LICENSES/MIT.txt) and [GPLv2](../LICENSES/GPL-2.0-only.txt).

The project does not track or merge future m1n1 development. Legacy protocol
identifiers and the `proxyclient/m1n1` Python package are retained for
compatibility with existing hosts and kernels; they do not imply an upstream
maintenance relationship.
