# gRPC-Go unary adapter

`Conn` implements `grpc.ClientConnInterface`. Existing generated clients can
replace `*grpc.ClientConn` with an LRPC connection without changing generated
protobuf code:

```go
conn, err := grpclrpc.Dial("/dev/ub_lrpc0", 1, map[string]uint32{
    searchpb.Search_Nearby_FullMethodName: 2,
})
client := searchpb.NewSearchClient(conn)
result, err := client.Nearby(ctx, request)
```

The adapter supports synchronous unary protobuf messages up to 60 KiB and a
Go shadow dispatcher through `ServeUnary`. The server callback stays on the
cgo system stack because the Go runtime cannot execute on a manually allocated
C E-stack; the Linux scheduler still switches into the shadow process's
separate address space. Streaming, deadlines that interrupt an in-flight
kernel handoff, and metadata remain separate work.
