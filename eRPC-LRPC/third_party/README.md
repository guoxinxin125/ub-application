# Pinned upstream sources

`scripts/fetch-deps.sh` obtains:

- Linux 6.6.155 LTS from kernel.org.
- eRPC commit `de83dab3eab4a0fb19bfc4881c11d4a6b89ff17d` from the official repository,
  plus `patches/erpc-lrpc.patch`.

Both trees are intentionally ignored by the parent repository.  Local LRPC
code lives outside them so a clean fetch can be rebuilt without hidden edits.

DeathStarBench is fetched with a blobless sparse checkout of the Hotel
Reservation Geo protobuf sources. The fetch script adds a nested minimal
`go.mod`, so building the Geo adapter does not resolve unrelated Consul,
MongoDB, or Memcached dependencies.
