module github.com/promisivia/shm-pcc-sdk/eRPC-LRPC/demo/deathstar-grpc

go 1.18

require (
	github.com/delimitrou/DeathStarBench/tree/master/hotelReservation/services/geo/proto v0.0.0
	github.com/promisivia/shm-pcc-sdk/eRPC-LRPC/adapters/grpc-go v0.0.0
	google.golang.org/protobuf v1.31.0
)

require (
	github.com/golang/protobuf v1.5.3 // indirect
	golang.org/x/net v0.9.0 // indirect
	golang.org/x/sys v0.7.0 // indirect
	golang.org/x/text v0.9.0 // indirect
	google.golang.org/genproto v0.0.0-20230410155749-daa745c078e1 // indirect
	google.golang.org/grpc v1.56.3 // indirect
)

replace github.com/delimitrou/DeathStarBench/tree/master/hotelReservation/services/geo/proto => ../../third_party/DeathStarBench/hotelReservation/services/geo/proto

replace github.com/promisivia/shm-pcc-sdk/eRPC-LRPC/adapters/grpc-go => ../../adapters/grpc-go
