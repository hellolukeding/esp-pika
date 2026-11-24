package data

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os/exec"
	"strings"
	"unicode/utf8"
)

const (
	relaySampleRateHz       = 16000
	relayMaxTTSSegmentRunes = 120
)

// WAVToPCM16kMono decodes a RIFF/WAVE PCM stream into 16 kHz mono signed 16-bit little-endian PCM.
func WAVToPCM16kMono(wav []byte) ([]byte, error) {
	if len(wav) < 12 || string(wav[0:4]) != "RIFF" || string(wav[8:12]) != "WAVE" {
		return nil, errors.New("invalid RIFF/WAVE header")
	}

	var (
		hasFmt        bool
		audioFormat   uint16
		channels      uint16
		sampleRate    uint32
		bitsPerSample uint16
		dataPayload   []byte
	)

	offset := 12
	for offset+8 <= len(wav) {
		chunkID := string(wav[offset : offset+4])
		chunkSize := int(binary.LittleEndian.Uint32(wav[offset+4 : offset+8]))
		payloadStart := offset + 8
		payloadEnd := payloadStart + chunkSize
		if chunkSize < 0 || payloadEnd > len(wav) {
			break
		}
		chunkData := wav[payloadStart:payloadEnd]

		switch chunkID {
		case "fmt ":
			if len(chunkData) < 16 {
				return nil, errors.New("invalid WAV fmt chunk")
			}
			audioFormat = binary.LittleEndian.Uint16(chunkData[0:2])
			channels = binary.LittleEndian.Uint16(chunkData[2:4])
			sampleRate = binary.LittleEndian.Uint32(chunkData[4:8])
			bitsPerSample = binary.LittleEndian.Uint16(chunkData[14:16])
			hasFmt = true
		case "data":
			dataPayload = chunkData
		}

		next := payloadEnd
		if chunkSize%2 == 1 && next < len(wav) {
			next++
		}
		offset = next
		if hasFmt && len(dataPayload) > 0 {
			break
		}
	}

	if !hasFmt || len(dataPayload) == 0 {
		return nil, errors.New("missing WAV fmt or data chunk")
	}
	if audioFormat != 1 || channels == 0 || sampleRate == 0 {
		return nil, fmt.Errorf("unsupported WAV format=%d channels=%d rate=%d", audioFormat, channels, sampleRate)
	}
	if bitsPerSample != 16 && bitsPerSample != 8 {
		return nil, fmt.Errorf("unsupported WAV bit depth=%d", bitsPerSample)
	}

	bytesPerSample := int(bitsPerSample / 8)
	frameStride := int(channels) * bytesPerSample
	if frameStride <= 0 || len(dataPayload) < frameStride {
		return nil, errors.New("empty WAV audio payload")
	}

	frameCount := len(dataPayload) / frameStride
	mono := make([]int16, frameCount)
	for i := 0; i < frameCount; i++ {
		var sum int32
		base := i * frameStride
		for ch := 0; ch < int(channels); ch++ {
			sampleOff := base + ch*bytesPerSample
			if bitsPerSample == 16 {
				sum += int32(int16(binary.LittleEndian.Uint16(dataPayload[sampleOff : sampleOff+2])))
			} else {
				sum += (int32(dataPayload[sampleOff]) - 128) << 8
			}
		}
		mono[i] = clampInt16(sum / int32(channels))
	}

	resampled := mono
	if sampleRate != relaySampleRateHz {
		resampled = resampleMonoLinear(mono, int(sampleRate), relaySampleRateHz)
	}

	out := make([]byte, len(resampled)*2)
	for i, sample := range resampled {
		binary.LittleEndian.PutUint16(out[i*2:i*2+2], uint16(sample))
	}
	return out, nil
}

func resampleMonoLinear(in []int16, srcRate, dstRate int) []int16 {
	if len(in) == 0 || srcRate <= 0 || dstRate <= 0 || srcRate == dstRate {
		return in
	}
	outLen := int(math.Round(float64(len(in)) * float64(dstRate) / float64(srcRate)))
	if outLen < 1 {
		outLen = 1
	}
	out := make([]int16, outLen)
	ratio := float64(srcRate) / float64(dstRate)
	for i := 0; i < outLen; i++ {
		pos := float64(i) * ratio
		idx0 := int(math.Floor(pos))
		if idx0 >= len(in)-1 {
			out[i] = in[len(in)-1]
			continue
		}
		frac := pos - float64(idx0)
		s0 := float64(in[idx0])
		s1 := float64(in[idx0+1])
		out[i] = clampInt16(int32(math.Round(s0 + (s1-s0)*frac)))
	}
	return out
}

func clampInt16(v int32) int16 {
	if v > math.MaxInt16 {
		return math.MaxInt16
	}
	if v < math.MinInt16 {
		return math.MinInt16
	}
	return int16(v)
}

// OggOpusToPCM16kMono decodes an OggOpus byte stream to 16 kHz mono signed 16-bit LE PCM via ffmpeg.
func OggOpusToPCM16kMono(ctx context.Context, ogg []byte) ([]byte, error) {
	if len(ogg) == 0 {
		return nil, errors.New("empty OggOpus input")
	}
	cmd := exec.CommandContext(
		ctx,
		"ffmpeg",
		"-hide_banner",
		"-loglevel", "error",
		"-f", "ogg",
		"-i", "pipe:0",
		"-ar", "16000",
		"-ac", "1",
		"-f", "s16le",
		"pipe:1",
	)
	cmd.Stdin = bytes.NewReader(ogg)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("ffmpeg ogg->pcm failed (%s): %w", strings.TrimSpace(stderr.String()), err)
	}
	pcm := stdout.Bytes()
	if len(pcm) == 0 || len(pcm)%2 != 0 {
		return nil, errors.New("ffmpeg produced invalid PCM output")
	}
	return pcm, nil
}

// PCM16kMonoToOggOpus encodes 16 kHz mono signed 16-bit LE PCM to a 24 kHz 20 ms OggOpus stream via ffmpeg.
func PCM16kMonoToOggOpus(ctx context.Context, pcm []byte) ([]byte, error) {
	if len(pcm) == 0 || len(pcm)%2 != 0 {
		return nil, errors.New("invalid PCM16 input")
	}
	cmd := exec.CommandContext(
		ctx,
		"ffmpeg",
		"-hide_banner",
		"-loglevel", "error",
		"-f", "s16le",
		"-ar", "16000",
		"-ac", "1",
		"-i", "pipe:0",
		"-c:a", "libopus",
		"-ar", "24000",
		"-ac", "1",
		"-frame_duration", "20",
		"-f", "ogg",
		"pipe:1",
	)
	cmd.Stdin = bytes.NewReader(pcm)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("ffmpeg pcm->ogg failed (%s): %w", strings.TrimSpace(stderr.String()), err)
	}
	out := stdout.Bytes()
	if len(out) == 0 {
		return nil, errors.New("ffmpeg produced empty OggOpus output")
	}
	return out, nil
}

// SplitTextForRelayTTS splits text into segments bounded by maxRunes without dropping sentence meaning.
func SplitTextForRelayTTS(text string, maxRunes int) []string {
	if maxRunes <= 0 {
		maxRunes = relayMaxTTSSegmentRunes
	}
	trimmed := strings.TrimSpace(text)
	if trimmed == "" {
		return nil
	}

	rawSentences := splitKeepDelimiters(trimmed, "。！？.!?\n；;")
	var result []string
	var cur strings.Builder
	curRunes := 0

	flushCur := func() {
		if part := strings.TrimSpace(cur.String()); part != "" {
			result = append(result, part)
		}
		cur.Reset()
		curRunes = 0
	}

	for _, sentence := range rawSentences {
		sentence = strings.TrimSpace(sentence)
		if sentence == "" {
			continue
		}
		sRunes := utf8.RuneCountInString(sentence)
		if curRunes > 0 && curRunes+sRunes <= maxRunes {
			cur.WriteString(sentence)
			curRunes += sRunes
			continue
		}
		flushCur()
		if sRunes <= maxRunes {
			cur.WriteString(sentence)
			curRunes = sRunes
			continue
		}
		for _, clause := range splitKeepDelimiters(sentence, "，,、 ") {
			clause = strings.TrimSpace(clause)
			if clause == "" {
				continue
			}
			cRunes := utf8.RuneCountInString(clause)
			if curRunes > 0 && curRunes+1+cRunes <= maxRunes {
				cur.WriteByte(' ')
				cur.WriteString(clause)
				curRunes += 1 + cRunes
				continue
			}
			flushCur()
			if cRunes <= maxRunes {
				cur.WriteString(clause)
				curRunes = cRunes
				continue
			}
			runes := []rune(clause)
			for i := 0; i < len(runes); i += maxRunes {
				end := min(i+maxRunes, len(runes))
				if part := strings.TrimSpace(string(runes[i:end])); part != "" {
					result = append(result, part)
				}
			}
		}
		flushCur()
	}
	flushCur()
	return result
}

func splitKeepDelimiters(input, delims string) []string {
	var parts []string
	var buf strings.Builder
	for _, r := range input {
		buf.WriteRune(r)
		if strings.ContainsRune(delims, r) {
			if p := strings.TrimSpace(buf.String()); p != "" {
				parts = append(parts, p)
			}
			buf.Reset()
		}
	}
	if p := strings.TrimSpace(buf.String()); p != "" {
		parts = append(parts, p)
	}
	return parts
}
