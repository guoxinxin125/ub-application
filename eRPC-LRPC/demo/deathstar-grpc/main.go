package main

import (
	"context"
	"fmt"
	"os"

	geopb "github.com/delimitrou/DeathStarBench/tree/master/hotelReservation/services/geo/proto"
	grpclrpc "github.com/promisivia/shm-pcc-sdk/eRPC-LRPC/adapters/grpc-go"
	"google.golang.org/protobuf/proto"
)

const (
	procedure = uint32(4)
	epoch     = uint64(1)
)

func controlDevice() string {
	if device := os.Getenv("LRPC_CONTROL_DEVICE"); device != "" {
		return device
	}
	return "/dev/ub_lrpc0"
}

func geoNearby(request []byte) ([]byte, error) {
	input := new(geopb.Request)
	if err := proto.Unmarshal(request, input); err != nil {
		return nil, err
	}
	if err := validateServiceState(); err != nil {
		return nil, err
	}
	result := &geopb.Result{HotelIds: []string{"1", "2", "3"}}
	if input.Lat == 0 && input.Lon == 0 {
		result.HotelIds = []string{"zero-location"}
	}
	return proto.Marshal(result)
}

func runClient() error {
	conn, err := grpclrpc.Dial(controlDevice(), epoch, map[string]uint32{
		geopb.Geo_Nearby_FullMethodName: procedure,
	})
	if err != nil {
		return err
	}
	defer conn.Close()
	client := geopb.NewGeoClient(conn)
	result, err := client.Nearby(context.Background(), &geopb.Request{
		Lat: 38.0235,
		Lon: -122.095,
	})
	if err != nil {
		return err
	}
	fmt.Printf("DEATHSTAR_GRPC_LRPC_RESULT method=%s hotels=%v\n",
		geopb.Geo_Nearby_FullMethodName, result.HotelIds)
	if len(result.HotelIds) != 3 || result.HotelIds[0] != "1" {
		return fmt.Errorf("unexpected Geo.Nearby response: %v", result.HotelIds)
	}
	fmt.Println("DEATHSTAR_GRPC_LRPC_PASS")
	return nil
}

func main() {
	if len(os.Args) == 2 && os.Args[1] == "shadow" {
		if err := openServiceState(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer closeServiceState()
		fmt.Println("DEATHSTAR_GRPC_SHADOW_START service=geo proc=4")
		if err := grpclrpc.ServeUnary(controlDevice(), procedure, epoch, geoNearby); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if err := runClient(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
