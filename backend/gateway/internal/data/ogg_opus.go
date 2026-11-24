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

// SplitOpusPackets expands any RFC 6716 multi-frame Opus packets into single-frame packets (code=0)
// so decoders capped at 60 ms (e.g., ESP-IDF esp_opus_dec default config) can decode 80-120 ms TTS packets.
func SplitOpusPackets(packets [][]byte) [][]byte {
	if len(packets) == 0 {
		return packets
	}
	out := make([][]byte, 0, len(packets))
	for _, pkt := range packets {
		out = append(out, SplitOpusPacketToSingleFrames(pkt)...)
	}
	return out
}

// SplitOpusPacketToSingleFrames splits a single RFC 6716 Opus packet into code=0 single-frame packets.
// Non-Opus or malformed packets are returned intact as a single-element slice.
func SplitOpusPacketToSingleFrames(pkt []byte) [][]byte {
	if len(pkt) == 0 {
		return nil
	}
	toc := pkt[0]
	code := toc & 0x03
	singleTOC := toc & 0xFC

	emitFrame := func(payload []byte) []byte {
		f := make([]byte, 1+len(payload))
		f[0] = singleTOC
		copy(f[1:], payload)
		return f
	}

	switch code {
	case 0:
		return [][]byte{pkt}
	case 1:
		rem := len(pkt) - 1
		if rem < 4 || rem%2 != 0 {
			return [][]byte{pkt}
		}
		fLen := rem / 2
		return [][]byte{
			emitFrame(pkt[1 : 1+fLen]),
			emitFrame(pkt[1+fLen:]),
		}
	case 2:
		pos := 1
		n1, ok := parseOpusFrameLength(pkt, &pos, len(pkt))
		n2 := len(pkt) - (pos + n1)
		if !ok || n1 < 2 || n2 < 2 {
			return [][]byte{pkt}
		}
		return [][]byte{
			emitFrame(pkt[pos : pos+n1]),
			emitFrame(pkt[pos+n1:]),
		}
	case 3:
		if len(pkt) < 2 {
			return [][]byte{pkt}
		}
		fc := pkt[1]
		vbr := (fc & 0x80) != 0
		hasPad := (fc & 0x40) != 0
		m := int(fc & 0x3F)
		if m <= 0 || m > 48 {
			return [][]byte{pkt}
		}
		pos := 2
		end := len(pkt)
		if hasPad {
			totalPad := 0
			for {
				if pos >= end {
					return [][]byte{pkt}
				}
				p := int(pkt[pos])
				pos++
				if p == 255 {
					totalPad += 254
					if totalPad > end-pos {
						return [][]byte{pkt}
					}
					continue
				}
				totalPad += p
				break
			}
			if end-pos < totalPad {
				return [][]byte{pkt}
			}
			end -= totalPad
		}
		if !vbr {
			rem := end - pos
			if rem%m != 0 {
				return [][]byte{pkt}
			}
			fLen := rem / m
			out := make([][]byte, 0, m)
			for i := 0; i < m; i++ {
				start := pos + i*fLen
				out = append(out, emitFrame(pkt[start:start+fLen]))
			}
			return out
		}
		lens := make([]int, m)
		sum := 0
		for i := 0; i < m-1; i++ {
			n, ok := parseOpusFrameLength(pkt, &pos, end)
			if !ok {
				return [][]byte{pkt}
			}
			lens[i] = n
			sum += n
		}
		if pos+sum > end {
			return [][]byte{pkt}
		}
		lens[m-1] = (end - pos) - sum
		out := make([][]byte, 0, m)
		cursor := pos
		for _, n := range lens {
			out = append(out, emitFrame(pkt[cursor:cursor+n]))
			cursor += n
		}
		return out
	default:
		return [][]byte{pkt}
	}
}

func parseOpusFrameLength(pkt []byte, pos *int, end int) (int, bool) {
	if *pos >= end {
		return 0, false
	}
	b0 := int(pkt[*pos])
	*pos++
	if b0 < 252 {
		return b0, true
	}
	if *pos >= end {
		return 0, false
	}
	b1 := int(pkt[*pos])
	*pos++
	return b0 + 4*b1, true
}
