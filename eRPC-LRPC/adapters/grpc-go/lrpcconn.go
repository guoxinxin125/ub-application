// Package grpclrpc provides a synchronous unary gRPC ClientConnInterface over
// UB-LRPC. Generated gRPC-Go clients can use Conn without source changes.
package grpclrpc

/*
#cgo CFLAGS: -I../../include -I../../include/uapi
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
*/
import "C"

import (
	"context"
	"fmt"
	"io"
	"runtime"
	"sync"
	"unsafe"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

const maxPayload = 60 * 1024

var (
	serverMu      sync.Mutex
	serverHandler func([]byte) ([]byte, error)
)

//export go_lrpc_dispatch
func go_lrpc_dispatch(payload unsafe.Pointer, requestSize C.size_t,
	responseSize *C.size_t) C.int {
	request := C.GoBytes(payload, C.int(requestSize))
	response, err := serverHandler(request)
	if err != nil || len(response) > maxPayload {
		return -1
	}
	if len(response) != 0 {
		C.memcpy(payload, unsafe.Pointer(&response[0]), C.size_t(len(response)))
	}
	*responseSize = C.size_t(len(response))
	return 0
}

// Conn maps full gRPC method names to published LRPC procedure IDs.
type Conn struct {
	handles map[uint32]*C.struct_lrpc_handle
	methods map[string]uint32
}

func Dial(device string, epoch uint64, methods map[string]uint32) (*Conn, error) {
	if len(methods) == 0 {
		return nil, fmt.Errorf("grpclrpc: empty method map")
	}
	cdev := C.CString(device)
	defer C.free(unsafe.Pointer(cdev))
	handles := make(map[uint32]*C.struct_lrpc_handle)
	copyMap := make(map[string]uint32, len(methods))
	for method, procedure := range methods {
		copyMap[method] = procedure
		if handles[procedure] != nil {
			continue
		}
		handles[procedure] = C.lrpc_go_open(
			cdev, C.uint32_t(procedure), C.uint64_t(epoch))
		if handles[procedure] == nil {
			for _, handle := range handles {
				C.lrpc_go_close(handle)
			}
			return nil, fmt.Errorf("grpclrpc: bind %s procedure %d: %v",
				device, procedure, io.ErrUnexpectedEOF)
		}
	}
	return &Conn{handles: handles, methods: copyMap}, nil
}

func (c *Conn) Close() {
	if c != nil {
		for procedure, handle := range c.handles {
			C.lrpc_go_close(handle)
			delete(c.handles, procedure)
		}
	}
}

func (c *Conn) Invoke(ctx context.Context, method string, args, reply any,
	_ ...grpc.CallOption) error {
	if err := ctx.Err(); err != nil {
		return status.FromContextError(err).Err()
	}
	procedure, ok := c.methods[method]
	if !ok {
		return status.Errorf(codes.Unimplemented,
			"grpclrpc: method %q is not mapped", method)
	}
	request, err := proto.Marshal(args.(proto.Message))
	if err != nil {
		return status.Errorf(codes.Internal, "marshal request: %v", err)
	}
	if len(request) > maxPayload {
		return status.Errorf(codes.ResourceExhausted,
			"request is %d bytes, maximum is %d", len(request), maxPayload)
	}
	response := make([]byte, maxPayload)
	responseSize := C.size_t(len(response))
	var requestPtr unsafe.Pointer
	if len(request) != 0 {
		requestPtr = unsafe.Pointer(&request[0])
	}
	rc := C.lrpc_go_invoke(c.handles[procedure], C.uint32_t(procedure), requestPtr,
		C.size_t(len(request)), unsafe.Pointer(&response[0]), &responseSize)
	if rc != 0 {
		return status.Errorf(codes.Internal, "LRPC procedure %d failed: %d",
			procedure, int(rc))
	}
	if err := ctx.Err(); err != nil {
		return status.FromContextError(err).Err()
	}
	if err := proto.Unmarshal(response[:int(responseSize)], reply.(proto.Message)); err != nil {
		return status.Errorf(codes.Internal, "unmarshal response: %v", err)
	}
	return nil
}

func (c *Conn) NewStream(context.Context, *grpc.StreamDesc, string,
	...grpc.CallOption) (grpc.ClientStream, error) {
	return nil, status.Error(codes.Unimplemented,
		"grpclrpc: streaming RPC is not supported")
}

// ServeUnary registers the current Go process as a passive shadow and blocks.
// One server is supported per process; handlers receive and return protobuf
// wire bytes so generated service implementations can dispatch them directly.
func ServeUnary(device string, procedure uint32, epoch uint64,
	handler func([]byte) ([]byte, error)) error {
	if handler == nil {
		return fmt.Errorf("grpclrpc: nil unary handler")
	}
	serverMu.Lock()
	defer serverMu.Unlock()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	serverHandler = handler
	cdev := C.CString(device)
	defer C.free(unsafe.Pointer(cdev))
	if rc := C.lrpc_go_serve(cdev, C.uint32_t(procedure), C.uint64_t(epoch)); rc != 0 {
		return fmt.Errorf("grpclrpc: shadow server stopped: %d", int(rc))
	}
	return nil
}
