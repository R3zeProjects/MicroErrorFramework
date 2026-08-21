# MESLS R1.2 reference audit

## Scope

The `0.3.0-beta` optimization work reviewed the user-provided local
`MESLS-R1.2` source snapshot as an engineering reference. The snapshot did not
contain Git metadata or an upstream URL, and no matching public repository was
found. This document therefore does not invent an unverifiable external link.

## Ideas evaluated

- pre-reserved contiguous storage for bounded hot-path buffers;
- `std::to_chars`-based formatting that avoids locale and stream overhead;
- short critical sections around shared state;
- ring-buffer and registry ownership patterns.
- exception-to-error capture and direct failed-result logging;
- code-based registry lookup and owning snapshots.

The snapshot was also built with Clang 22.1.6 in C++23 Release mode. Its normal
configuration passed 10/10 CTest tests. Enabling its own `MESLS_WERROR` option
correctly exposed an API defect: `Logger` explicitly defaults move construction
and assignment even though its `std::shared_mutex` member makes both operations
implicitly deleted. MicroErrorFramework does not copy that declaration pattern;
the external snapshot was not modified during this audit.

MicroErrorFramework already used stack formatting and `std::to_chars`. The
transferable storage idea was applied to the async logger as two pre-reserved
vectors that swap between the producer side and backend consumer. In the
200,000-record allocation workload this reduced observed allocation bytes from
33,860,624 to 27,307,808 (−19.4%).

For `0.4.0-beta`, the relevant error-control capabilities were adapted to the
existing MEF API: `attempt()`, `Logger::capture()`, direct failed-Result logging,
and code-keyed owning registry queries. They were not copied as MESLS wrappers.

## Ideas rejected

- Returning registry references after releasing a lock was rejected because a
  concurrent mutation can invalidate the reference.
- Consumer-side prefetch of arbitrary scalar tasks was rejected after the
  cancellation test demonstrated that prefetched work becomes invisible to
  `clear_queue()`.
- A general spin lock was rejected because five-run medians reduced overall
  dispatch gain and regressed bulk throughput.
- `StringVariable`, `.mlog`, `FSaver`, and generic value serialization were not
  moved into MEF. They belong to a separate persistence/serialization library
  that can integrate through `ILogSink` as part of the same ecosystem.

The final bulk optimization is limited to callbacks submitted through the
explicit bulk API, has a fixed claim bound of 16, and is documented as a public
0.3.x concurrency contract. The owning registry observation rules are a public
0.4.x concurrency contract.
