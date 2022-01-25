// Code generated by cmd/cgo -godefs; DO NOT EDIT.
// cgo -godefs -- -fsigned-char skbtracer_types.go

package ebpf

type Tuple struct {
	Sport     uint16
	Dport     uint16
	Family    uint8
	Protocol  uint8
	Pad_cgo_0 [2]byte
	Saddr     In6Addr
	Daddr     In6Addr
}
type SocketInfo struct {
	Ns        uint64
	Tgid      uint32
	Netns     uint32
	Direction uint8
	Family    uint8
	Protocol  uint8
	Pad_cgo_0 [5]byte
}
type FlowStats struct {
	Last_update uint64
	Sent_bytes  uint64
	Recv_bytes  uint64
}
type UDPFlow struct {
	Tup Tuple
	Sk  uint64
}

type UDPCloseEvent struct {
	Sk     uint64
	Skinfo SocketInfo
}
type In6Addr struct {
	U [16]byte
}