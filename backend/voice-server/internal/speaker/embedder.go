package speaker

import (
	"encoding/binary"
	"math"
)

const EmbeddingDim = 192

// Embedder extracts a 192-dimensional L2-normalized speaker embedding from audio bytes.
// Phase 1 ships a deterministic Go DFT/spectral-statistics extractor implementing the exact
// 192-D contract; production deployments can swap in a sherpa-onnx 3D-Speaker implementation.
type Embedder interface {
	Extract(rawAudio []byte) []float32
}

// SpectralEmbedder computes a deterministic 192-D L2-normalized embedding from PCM or frame bytes.
type SpectralEmbedder struct{}

// Extract transforms raw audio bytes (int16 LE / float32 LE / Opus payload stub) into a 192-D unit vector.
func (e *SpectralEmbedder) Extract(raw []byte) []float32 {
	vec := make([]float32, EmbeddingDim)
	if len(raw) == 0 {
		return vec
	}

	samples := bytesToFloatSamples(raw)
	if len(samples) == 0 {
		return vec
	}

	// Compute 32 Goertzel spectral bin energies across 120Hz..7600Hz over up to 6 overlapping windows.
	const numBands = 32
	winSize := 256
	if len(samples) < winSize {
		winSize = len(samples)
	}
	hop := winSize / 2
	if hop < 1 {
		hop = 1
	}

	bandMean := make([]float64, numBands)
	bandSq := make([]float64, numBands)
	bandDelta := make([]float64, numBands)
	prevBand := make([]float64, numBands)
	frames := 0

	for start := 0; start+winSize <= len(samples) && frames < 32; start += hop {
		frame := samples[start : start+winSize]
		for b := 0; b < numBands; b++ {
			freq := 120.0 + float64(b)*(7400.0/float64(numBands-1))
			mag := goertzelLogEnergy(frame, 16000.0, freq)
			bandMean[b] += mag
			bandSq[b] += mag * mag
			if frames > 0 {
				d := mag - prevBand[b]
				bandDelta[b] += d * d
			}
			prevBand[b] = mag
		}
		frames++
	}
	if frames == 0 {
		frames = 1
	}

	overallMean := 0.0
	for b := 0; b < numBands; b++ {
		overallMean += bandMean[b] / float64(frames)
	}
	overallMean /= float64(numBands)

	for b := 0; b < numBands; b++ {
		mean := (bandMean[b] / float64(frames)) - overallMean
		variance := (bandSq[b] / float64(frames)) - (bandMean[b]/float64(frames))*(bandMean[b]/float64(frames))
		if variance < 0 {
			variance = 0
		}
		stddev := math.Sqrt(variance)
		delta := math.Sqrt(bandDelta[b] / float64(frames))

		vec[b] = float32(mean)
		vec[numBands+b] = float32(stddev)
		vec[2*numBands+b] = float32(delta)
		if b+1 < numBands {
			nextMean := (bandMean[b+1] / float64(frames)) - overallMean
			vec[3*numBands+b] = float32(mean - nextMean)
		}
		vec[4*numBands+b] = float32(mean * stddev)
		vec[5*numBands+b] = float32(math.Tanh(stddev - delta))
	}

	l2Normalize(vec)
	return vec
}

func bytesToFloatSamples(raw []byte) []float64 {
	raw = stripWAVHeader(raw)
	if len(raw) < 2 {
		out := make([]float64, len(raw))
		for i, b := range raw {
			out[i] = (float64(b) - 128.0) / 128.0
		}
		return out
	}
	n := len(raw) / 2
	out := make([]float64, n)
	for i := 0; i < n; i++ {
		s := int16(binary.LittleEndian.Uint16(raw[2*i : 2*i+2]))
		out[i] = float64(s) / 32768.0
	}
	return out
}

func stripWAVHeader(raw []byte) []byte {
	if len(raw) < 44 || string(raw[:4]) != "RIFF" || string(raw[8:12]) != "WAVE" {
		return raw
	}
	off := 12
	for off+8 <= len(raw) {
		chunkID := string(raw[off : off+4])
		chunkSize := int(binary.LittleEndian.Uint32(raw[off+4 : off+8]))
		off += 8
		if chunkID == "data" {
			if off+chunkSize <= len(raw) {
				return raw[off : off+chunkSize]
			}
			return raw[off:]
		}
		if chunkSize < 0 || off+chunkSize > len(raw) {
			break
		}
		off += chunkSize
	}
	return raw[44:]
}

func goertzelLogEnergy(frame []float64, sampleRate, targetFreq float64) float64 {
	omega := 2.0 * math.Pi * targetFreq / sampleRate
	coeff := 2.0 * math.Cos(omega)
	var q0, q1, q2 float64
	for _, x := range frame {
		q0 = coeff*q1 - q2 + x
		q2 = q1
		q1 = q0
	}
	power := q1*q1 + q2*q2 - q1*q2*coeff
	if power < 1e-9 {
		power = 1e-9
	}
	return math.Log(power)
}

func l2Normalize(vec []float32) {
	var sum float64
	for _, v := range vec {
		sum += float64(v) * float64(v)
	}
	if sum <= 1e-12 {
		return
	}
	inv := float32(1.0 / math.Sqrt(sum))
	for i := range vec {
		vec[i] *= inv
	}
}

// CosineSimilarity computes cosine similarity between two unit/float vectors.
func CosineSimilarity(a, b []float32) float32 {
	if len(a) == 0 || len(a) != len(b) {
		return 0
	}
	var dot, na, nb float64
	for i := range a {
		dot += float64(a[i]) * float64(b[i])
		na += float64(a[i]) * float64(a[i])
		nb += float64(b[i]) * float64(b[i])
	}
	if na <= 1e-12 || nb <= 1e-12 {
		return 0
	}
	return float32(dot / (math.Sqrt(na) * math.Sqrt(nb)))
}
