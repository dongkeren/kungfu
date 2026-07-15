# Project Cut protocol contract

`framework/project-cut` is the build-free, content-addressed protocol layer that
binds one declared source projection, one Xinfa Atlas, and an admitted Kungfu
Episode delta. It implements [ADR-0097](../../docs/adr/ADR-0097-project-cut-spacetime-and-publication-boundary.md)
and [ADR-0098](../../docs/adr/ADR-0098-project-cut-v1-canonical-root-and-source-projection.md).
The agent-first settlement surface implements
[ADR-0101](../../docs/adr/ADR-0101-project-cut-agent-first-settlement.md)
without changing the frozen `project.cut/v1` root contract.

The layer owns no source, Atlas, Episode, Mission, Go, or Git authority. It
validates references to those authorities and computes four deliberately
separate identities:

- `cutRoot`: SHA-256 of the canonical `project.cut.root-input/v1` semantic
  preimage, excluding `cutRoot`, receipts, publication coordinates, and the
  containing Git commit OID;
- `serializationRoot`: SHA-256 of canonical `project.cut/v1` JSON, including
  `cutRoot`;
- `artifactDigest`: SHA-256 of the exact artifact bytes that were inspected;
  and
- `receiptRoot`: SHA-256 of the receipt preimage.

`sha256-project-cut-canonical-json-v1` hashes canonical JSON without a trailing
newline. Canonical JSON sorts object keys by UTF-8 bytes, preserves
schema-declared array order, admits valid NFC strings only, and admits
non-negative safe integers only. Set-like arrays must already be UTF-8 byte
sorted and unique; the verifier rejects ambiguous input rather than silently
repairing it. Exact artifact bytes are hashed separately and may include a
presentation newline.

The source projection policy permits declared `.xinfa` and `.kungfu` authority
inputs but rejects Git internals, runtime/cache/index/generated state, private
raw payloads, and `.kungfu/project-cuts` protocol output. Paths are NFC POSIX
relative paths. This prevents a generated Project Cut from feeding its own
source root without broadly hiding user-declared authority material.

The zero-dependency API is in [`src/project-cut.mjs`](src/project-cut.mjs):

```js
import {
  buildProjectCut,
  buildSourceProjection,
  createProjectCutReceipt,
  verifyProjectCut,
  verifyProjectCutReceipt,
  verifySourceProjection,
} from './framework/project-cut/src/project-cut.mjs';
```

Run the protocol and settlement contracts, golden roots, receipts, negative
fixtures, and a real Xinfa successor-Atlas integration:

```sh
./shifu check:project-cut-settlement
./shifu test:project-cut-settlement
./shifu test:project-cut-settlement:integration
./shifu check:source
```

The settlement CLI reads the Git index as the source candidate and defaults to
dry-run. Only `--execute` writes the Atlas promotion and content-addressed cut;
only `--stage` adds those exact paths to the index. It never commits or pushes:

```sh
./shifu project-cut prepare --request settlement-request.json --json
./shifu project-cut prepare --request settlement-request.json --execute --stage --json
./shifu project-cut verify --state .kungfu/runtime/project-cut/settlements/<cut>/state.json --json
./shifu project-cut commit-observe --state .kungfu/runtime/project-cut/settlements/<cut>/state.json --commit HEAD --execute --json
./shifu project-cut reconcile --commit HEAD --json
```

`hooks/project-cut-hook.mjs` is an optional thin adapter. Point
`PROJECT_CUT_SETTLEMENT_STATE` at local rebuildable state and invoke it with
`pre-commit` or `post-commit`; it only calls the same public verify/observe
core, performs no compile or network access, and explicitly reports
`authority: false`. Skipped or absent hooks do not create proof: `reconcile`
is the stage-0, headless recovery path from tracked Git JSON/JSONL.

Optional JSON Schema validation runs when repository dependencies are present;
the semantic/root verifier and settlement core use only Node built-ins and
remain available for stage-0 recovery.
