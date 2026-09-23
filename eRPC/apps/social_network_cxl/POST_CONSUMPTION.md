# Complete Post consumption (implementation steps 1-5)

UB Home/User Timeline now stop their existing timers after importing and
consuming all effective Post fields. DmRPC's RMEM reader stops after completed
RDMA reads, Post protobuf parsing and the same field checksum. Compose retains
its existing short-path timing boundary, but UB's inline request size changes.
This is an application-consumption metric, not just a transport latency.

The current native layout is version 4 and exactly 2048 bytes. Every array has
a compile-time position: 1168 bytes for text, 10 media, 5 mentions and 5 URLs.
The contiguous 104-byte prefix holds scalar fields, counts and every string
length. The reader copies that prefix once logically and then addresses each
effective field with `offsetof(PostData, field)`; there is no remote offset
lookup. Fixed arrays keep a trailing NUL for local service code. Capacity
overflow fails explicitly rather than truncating data.

Both active client generators use 500 initial random text bytes, preserving
5 appended mentions, 5 appended URLs and 10 media. With the test's three-digit
user IDs, the complete fixture has 921 text bytes and fits all fixed fields.

Canonical field order: post_id, req_id, timestamp, post_type, creator user_id,
creator username, text, media count and each ID/type, mention count and each
ID/username, URL count and each shortened/expanded string. Integers use their
numeric value; strings use byte length and little-endian 8-byte words plus a
bounded tail. Protobuf absence uses getter defaults. Padding, terminators and
unused array capacity do not participate. The inexpensive checksum is not a
cryptographic integrity check. Both applications ship identical
post_checksum.h files to remain independently buildable.

UB copies only the fixed metadata prefix; effective string bytes are loaded
into checksum accumulators in logical 64-byte blocks. Small constant-size
memcpy calls implement alias-safe loads, not full-post materialization.
Generated instructions and remote transaction granularity depend on the target
compiler and memory mapping; inspect the AArch64 build before drawing conclusions.
The per-thread volatile sink plus compiler barrier keeps consumption before
the timer. Data must remain immutable and its transferred reference live until
consumption finishes. Invalid UB metadata releases the imported reference before
propagating an error; invalid RDMA protobuf is not recorded as a successful read.

Local test without protobuf:

```bash
g++ -std=c++11 -O3 -DNDEBUG -Wall -Wextra -Werror -pedantic \
  tests/post_consume_test.cpp -o /tmp/post_consume_test
/tmp/post_consume_test
```

Run from this directory. Without SN_TEST_PROTOBUF the test uses a getter-shaped
fixture, not actual protobuf parsing. For a real protobuf round-trip, generate
fresh files into a temporary directory on the Linux host:

```bash
test_dir=$(mktemp -d /tmp/sn-consume.XXXXXX)
protoc -I . --cpp_out="$test_dir" social_network.proto
g++ -std=c++11 -O3 -DSN_TEST_PROTOBUF -I"$test_dir" \
  tests/post_consume_test.cpp "$test_dir/social_network.pb.cc" \
  $(pkg-config --cflags --libs protobuf) -pthread -o "$test_dir/test"
"$test_dir/test"
```

Coverage includes default/empty fields, embedded NUL, UTF-8, 63/64/65-byte
boundaries, maximum text capacity, unaligned input, unused slots, overflow
rejection, mention/URL consumption, invalid layout/length/count rejection and
request ABI. A complete Compose fixture retains every field.
Compare the two post_checksum.h files before deploying.

Changed production files under eRPC/apps/social_network_cxl:

- post_data.h; post_checksum.h (new); post_consume.h (new)
- client/client.h; client/client.cpp
- post_storage/post_storage.cpp
- compose_post/compose_post.h; compose_post/compose_post.cpp
- social_network_rpc_type.h
- user_mention/user_mention.cpp; url_shorten/url_shorten.cpp
- user_service/user_service.cpp; home_timeline/home_timeline.h

DmRPC/cn/app/social_network: client/client.cpp, client/client.h and post_checksum.h (new).
Documentation/tests: README_UB.md, this file, tests/post_consume_test.cpp (new),
and DmRPC's POST_CONSUMPTION.md (new).

This change does not align datasets/request replay or introduce new diagnostic
histograms. Empty-result behavior and post selection remain as before; verify
identical successful post IDs and content separately before making a fairness
claim. The fixed capacities have not been checked against a live MongoDB dump;
overflow is an explicit error. Windows unit tests are not UB/RDMA integration
validation. Both Linux deployments still need rebuilds and hardware tests.
