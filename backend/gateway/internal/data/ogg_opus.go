package data

import (
	"bytes"
	"encoding/binary"
)

var oggCRCTable = func() [256]uint32 {
	var t [256]uint32
	for i := 0; i < 256; i++ {
		r := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if r&0x80000000 != 0 {
				r = (r << 1) ^ 0x04c11db7
			} else {
				r <<= 1
			}
		}
		t[i] = r
	}
	return t
}()

func oggCRC(data []byte) uint32 {
	var crc uint32
	for _, b := range data {
		crc = (crc << 8) ^ oggCRCTable[byte(crc>>24)^b]
	}
	return crc
}

// BuildOggOpus wraps raw 60ms mono Opus frames into a valid RFC 7845 OggOpus container.
func BuildOggOpus(frames [][]byte, sampleRate uint32, frameDurationMS uint32) []byte {
	if sampleRate == 0 {
		sampleRate = 16000
	}
	if frameDurationMS == 0 {
		frameDurationMS = 60
	}
	const serial uint32 = 0x4550494B // "EPIK"
	var out bytes.Buffer

	// Page 0: OpusHead (BOS)
	var head bytes.Buffer
	head.WriteString("OpusHead")
	head.WriteByte(1) // version
	head.WriteByte(1) // mono channel
	_ = binary.Write(&head, binary.LittleEndian, uint16(312))
	_ = binary.Write(&head, binary.LittleEndian, sampleRate)
	_ = binary.Write(&head, binary.LittleEndian, int16(0)) // output gain
	head.WriteByte(0)                                      // mapping family 0
	writeOggPage(&out, 0x02, 0, serial, 0, [][]byte{head.Bytes()})

	// Page 1: OpusTags
	var tags bytes.Buffer
	tags.WriteString("OpusTags")
	vendor := "esp-pika"
	_ = binary.Write(&tags, binary.LittleEndian, uint32(len(vendor)))
	tags.WriteString(vendor)
	_ = binary.Write(&tags, binary.LittleEndian, uint32(0)) // 0 user comments
	writeOggPage(&out, 0x00, 0, serial, 1, [][]byte{tags.Bytes()})

	if len(frames) == 0 {
		return out.Bytes()
	}

	// OggOpus granule positions always advance at 48kHz ticks regardless of input sample rate.
	granuleStep := uint64(48 * frameDurationMS)
	var granule uint64
	seq := uint32(2)
	for i, pkt := range frames {
		if len(pkt) == 0 {
			continue
		}
		granule += granuleStep
		var hdrType byte
		if i == len(frames)-1 {
			hdrType = 0x04 // EOS
		}
		writeOggPage(&out, hdrType, granule, serial, seq, [][]byte{pkt})
		seq++
	}
	return out.Bytes()
}

func writeOggPage(dst *bytes.Buffer, hdrType byte, granule uint64, serial, seq uint32, packets [][]byte) {
	var segTable bytes.Buffer
	var payload bytes.Buffer
	for _, pkt := range packets {
		rem := len(pkt)
		for rem >= 255 {
			segTable.WriteByte(255)
			rem -= 255
		}
		segTable.WriteByte(byte(rem))
		payload.Write(pkt)
	}

	var page bytes.Buffer
	page.WriteString("OggS")
	page.WriteByte(0)
	page.WriteByte(hdrType)
	_ = binary.Write(&page, binary.LittleEndian, granule)
	_ = binary.Write(&page, binary.LittleEndian, serial)
	_ = binary.Write(&page, binary.LittleEndian, seq)
	_ = binary.Write(&page, binary.LittleEndian, uint32(0)) // CRC placeholder
	page.WriteByte(byte(segTable.Len()))
	page.Write(segTable.Bytes())
	page.Write(payload.Bytes())

	raw := page.Bytes()
	crc := oggCRC(raw)
	binary.LittleEndian.PutUint32(raw[22:26], crc)
	dst.Write(raw)
}

// ExtractOggOpusPackets parses an OggOpus byte stream and returns raw Opus audio packets,
// skipping OpusHead/OpusTags header packets. If raw is not Ogg-framed, returns [raw].
func ExtractOggOpusPackets(raw []byte) [][]byte {
	if len(raw) < 27 || string(raw[:4]) != "OggS" {
		if len(raw) == 0 {
			return nil
		}
		return [][]byte{raw}
	}

	var packets [][]byte
	var curPkt bytes.Buffer
	off := 0
	for off+27 <= len(raw) {
		if string(raw[off:off+4]) != "OggS" {
			break
		}
		numSegs := int(raw[off+26])
		if off+27+numSegs > len(raw) {
			break
		}
		segs := raw[off+27 : off+27+numSegs]
		dataOff := off + 27 + numSegs
		for _, segLen := range segs {
			l := int(segLen)
			if dataOff+l > len(raw) {
				return packets
			}
			curPkt.Write(raw[dataOff : dataOff+l])
			dataOff += l
			if l < 255 {
				pkt := make([]byte, curPkt.Len())
				copy(pkt, curPkt.Bytes())
				curPkt.Reset()
				if !isOpusHeaderPacket(pkt) {
					packets = append(packets, pkt)
				}
			}
		}
		off = dataOff
	}
	return packets
}

func isOpusHeaderPacket(pkt []byte) bool {
	return bytes.HasPrefix(pkt, []byte("OpusHead")) || bytes.HasPrefix(pkt, []byte("OpusTags"))
}
