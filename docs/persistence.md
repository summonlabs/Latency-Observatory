# Persistence

The store is a directory of segments plus a manifest. All multi byte integers
are little endian and written explicitly, so the format does not depend on the
host. Record payloads are canonical JSON; the framing around them is binary and
integrity checked.

## Manifest: `manifest.lobs`

```
magic=LATOBS-STORE
format=1
created_ns=<utc nanoseconds>
policy_digest=<sha256 of the canonical policy>
crc32c=<crc32c of every byte above>
```

The manifest is written once, when the directory is created. Opening a store
verifies the checksum first and the format version second:

* a checksum mismatch is `integrity_failure`;
* a format version other than the supported one is `version_mismatch`, and the
  store is not opened at all.

## Segments: `seg-XXXXXXXX.lobs`

The file name encodes the segment identifier in hexadecimal. A segment is
created lazily on the first append, so a runtime that only reads leaves no empty
file behind.

### Header (48 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `LOBSSEG1` |
| 8 | 2 | format version |
| 10 | 2 | header size (48) |
| 12 | 4 | segment identifier |
| 16 | 8 | creation time (UTC nanoseconds) |
| 24 | 4 | record count hint |
| 28 | 4 | flags |
| 32 | 4 | CRC-32C over bytes 0..31 |
| 36 | 12 | reserved |

### Record

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | payload length |
| 4 | 1 | record type |
| 5 | 1 | flags |
| 6 | 2 | reserved |
| 8 | 4 | CRC-32C of the payload |
| 12 | n | canonical JSON payload |

Record types: `source`, `endpoint`, `link`, `queue`, `hop`, `generation`,
`path`, `clock_domain`, `clock_sync`, `measurement_current`,
`measurement_historical`, `baseline`, `fence_state`.

A record payload can never start with the footer magic: the first four bytes
would have to encode the length `LOBSS`, which exceeds the maximum payload size.

### Footer (40 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `LOBSEND1` |
| 8 | 4 | record count |
| 12 | 4 | reserved |
| 16 | 8 | payload bytes |
| 24 | 4 | CRC-32C over bytes 0..23 |
| 28 | 4 | reserved |
| 32 | 8 | rolling segment digest |

The rolling digest folds every payload in the segment
(`digest = digest * FNV_prime ^ fnv1a64(payload)`), which makes a segment
verifiable as a unit.

## Rotation and capacity

Segments are rotated when the next record would exceed `max_segment_records` or
`max_segment_bytes`. A record that cannot fit inside a segment at all is
refused. When the projected store size would exceed `max_store_bytes` the append
fails with `capacity_exceeded`: the runtime refuses new observations instead of
dropping old ones or growing without bound.

## Recovery

Recovery reads segments in file name order and stops at the first problem:

| Condition | Outcome |
| --- | --- |
| missing manifest | the store is created when the writer is allowed to create one, otherwise `not_found` |
| manifest checksum mismatch | `integrity_failure` |
| unsupported format version | `version_mismatch` |
| bad segment magic, format, header size or header CRC | the segment and everything after it are rejected |
| record header or payload truncated | reading stops; records before the damage are kept |
| record CRC mismatch | reading stops; the damaged record is never interpreted |
| unknown record type | reading stops |
| footer CRC, payload byte count or record count mismatch | the segment is reported as truncated |
| segment that yielded no record and has no footer | treated as empty rather than truncated: no evidence was lost |

Everything discovered is reported in a `RecoveryReport` (segments scanned,
accepted, rejected, records read and discarded, bytes read, truncation detail and
typed reasons). The report is exposed through `status` and is attached to every
explanation when recovery was not clean. Recovery never rewrites, repairs or
guesses: it discards the damaged tail.

## Restart semantics

Loading is not "resuming". On load:

* definitions (sources, endpoints, links, queues, hops, generations, paths,
  clock domains) are re-registered in write order, so identities are identical;
* clock synchronization reports are restored with their original observation
  times, which means they are usually expired: a restarted runtime cannot judge
  cross domain evidence fresh;
* measurement records keep their `observed_at` and `received_at` values, keep
  their recorded evidence state, and get their freshness recomputed against the
  session start of the new process, with the reason `restart_loaded_evidence`;
* fence state (sessions, last sequence, bounded replay window, revisions) is
  restored, so replayed sequences are still refused;
* baselines are restored with their generation, clock domain, histogram
  specification, probes and policy digest.

The consequence is the property the tests assert: after a restart, the same
window reports zero *current* exchanges and the full *historical* distribution,
with the original observation and receive times intact.

## Versioning policy

The format version changes when the framing or the payload schema changes in a
way an older build cannot read. A newer store is never guessed at; an older store
is read by the new build when the payload schema stayed compatible. The policy
digest is recorded so that evidence can be interpreted against the policy that
produced it.
