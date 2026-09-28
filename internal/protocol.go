// Package internal: HP-PRO 协议（逆向自 hppro 5.0 libgojni.so）
// 帧格式: [4B BE magic=0x00001a0a][4B BE payloadLen][protobuf payload]
package internal

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	Magic       = 0x00001a0a
	MaxFrameLen = 4 * 1024 * 1024
)

func appendVarint(b []byte, v uint64) []byte {
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

func appendTag(b []byte, field int, wire int) []byte {
	return appendVarint(b, uint64(field)<<3|uint64(wire))
}

func writeFrame(out []byte, payload []byte) []byte {
	out = append(out, 0x00, 0x00, 0x1a, 0x0a)
	var l [4]byte
	binary.BigEndian.PutUint32(l[:], uint32(len(payload)))
	out = append(out, l[:]...)
	out = append(out, payload...)
	return out
}

func readFrame(r io.Reader) ([]byte, error) {
	var hdr [8]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	if binary.BigEndian.Uint32(hdr[0:4]) != Magic {
		return nil, errors.New("bad magic")
	}
	n := binary.BigEndian.Uint32(hdr[4:8])
	if n > MaxFrameLen {
		return nil, errors.New("frame too large")
	}
	payload := make([]byte, n)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

type fieldVisitor func(field int, wire int, val []byte)

func parseFields(b []byte, visit fieldVisitor) error {
	p := 0
	for p < len(b) {
		tag, n := readUvarint(b[p:])
		if n <= 0 {
			return errors.New("bad tag")
		}
		p += n
		field := int(tag >> 3)
		wire := int(tag & 7)
		var val []byte
		switch wire {
		case 0:
			v, n := readUvarint(b[p:])
			if n <= 0 {
				return errors.New("bad varint")
			}
			p += n
			val = appendVarint(nil, v)
		case 2:
			l, n := readUvarint(b[p:])
			if n <= 0 || p+n+int(l) > len(b) {
				return errors.New("bad length")
			}
			p += n
			val = b[p : p+int(l)]
			p += int(l)
		default:
			return fmt.Errorf("unsupported wire %d", wire)
		}
		visit(field, wire, val)
	}
	return nil
}

func readUvarint(b []byte) (uint64, int) {
	var v uint64
	var shift uint
	for i, c := range b {
		v |= uint64(c&0x7f) << shift
		if c < 0x80 {
			return v, i + 1
		}
		shift += 7
		if shift > 63 {
			return 0, 0
		}
	}
	return 0, 0
}

func readVarint(b []byte) uint64 {
	v, _ := readUvarint(b)
	return v
}

type StreamFramer struct {
	buf []byte
}

func (sf *StreamFramer) Feed(data []byte) [][]byte {
	sf.buf = append(sf.buf, data...)
	var out [][]byte
	for {
		if len(sf.buf) < 8 {
			break
		}
		magic := uint32(sf.buf[0])<<24 | uint32(sf.buf[1])<<16 | uint32(sf.buf[2])<<8 | uint32(sf.buf[3])
		if magic != Magic {
			sf.buf = sf.buf[1:]
			continue
		}
		n := binary.BigEndian.Uint32(sf.buf[4:8])
		if n > MaxFrameLen {
			sf.buf = sf.buf[8:]
			continue
		}
		if uint32(len(sf.buf)) < 8+n {
			break
		}
		out = append(out, append([]byte(nil), sf.buf[8:8+n]...))
		sf.buf = sf.buf[8+n:]
	}
	return out
}
