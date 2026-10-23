# Board protocol definition

- `ns_board.proto`: the messages a board exchanges with the phone app. This is
  the source of truth; `docs/protocol.md` describes the GATT layout, framing and
  flows around it.
- `ns_board.options`: nanopb size limits, so the firmware never allocates.

## Regenerating the firmware code

```sh
brew install protobuf          # protoc
scripts/gen-proto.sh           # writes firmware/components/ns_proto/ns_board.pb.{c,h}
scripts/gen-proto.sh --check   # fails if the committed code is stale
```

The script installs the nanopb generator pinned in
`scripts/requirements-proto.txt` into `.venv/` (gitignored) on first use. The
nanopb runtime in `firmware/components/ns_proto/nanopb/` is vendored at the
same version. Generated files are committed.

## Wire fixtures

`test/host/proto_roundtrip/` encodes one message of each kind with nanopb,
decodes it back, and compares the bytes with `test/fixtures/proto/*.bin`:

```sh
make -C test/host/proto_roundtrip            # check
make -C test/host/proto_roundtrip fixtures   # rewrite after a deliberate change
```

The iOS app keeps a copy of `ns_board.proto` and of these fixtures, and its
tests decode the fixtures with SwiftProtobuf. After changing the proto, run the
app repository's `scripts/sync-proto.sh` to update its copy and generated code.
