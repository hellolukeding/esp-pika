package biz

import (
	"context"
	"regexp"
	"strings"
	"sync"
)

// AudioPriority defines speaker playback preemption levels per architecture.md §8.3.3.
type AudioPriority int

const (
	AudioPriorityAlert        AudioPriority = 0 // P0: Security / emergency alerts
	AudioPriorityConversation AudioPriority = 1 // P1: Active real-time voice turn
	AudioPriorityNotification AudioPriority = 2 // P2: Async background completion
)

// ASRStream accepts streaming 16kHz Opus uplink frames and produces a finalized transcript.
type ASRStream interface {
	WriteOpusFrame(ctx context.Context, frame []byte) error
	Finish(ctx context.Context) (string, error)
	Close() error
}

// ASRProvider creates per-utterance streaming speech recognition sessions.
type ASRProvider interface {
	StartStream(ctx context.Context, deviceID string) (ASRStream, error)
}

// TTSProvider synthesizes spoken text chunks into 24kHz Opus downlink frames using a speaker voice.
type TTSProvider interface {
	Synthesize(ctx context.Context, text, voice string) ([][]byte, error)
}

// VoiceReviewer performs cloud gray-zone speaker verification (e.g. 3D-Speaker + Qdrant).
type VoiceReviewer interface {
	Review(ctx context.Context, deviceID string, opusFrames [][]byte) (speakerID string, role SpeakerRole, confidence float32, err error)
}

// SpeakerArbitrator enforces P0 Alert > P1 Conversation > P2 Notification per physical speaker.
type SpeakerArbitrator struct {
	mu     sync.Mutex
	active map[string]speakerLease
}

type speakerLease struct {
	turnID   string
	priority AudioPriority
}

// NewSpeakerArbitrator constructs a SpeakerArbitrator.
func NewSpeakerArbitrator() *SpeakerArbitrator {
	return &SpeakerArbitrator{
		active: make(map[string]speakerLease),
	}
}

// Acquire grants playback ownership if no higher-priority turn currently holds the device speaker.
func (a *SpeakerArbitrator) Acquire(deviceID, turnID string, prio AudioPriority) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if cur, ok := a.active[deviceID]; ok && cur.turnID != turnID && prio > cur.priority {
		return false
	}
	a.active[deviceID] = speakerLease{turnID: turnID, priority: prio}
	return true
}

// Release frees the speaker lease if still held by turnID.
func (a *SpeakerArbitrator) Release(deviceID, turnID string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if cur, ok := a.active[deviceID]; ok && cur.turnID == turnID {
		delete(a.active, deviceID)
	}
}

var (
	reCodeFence = regexp.MustCompile("(?s)```.*?```")
	reMDLink    = regexp.MustCompile(`\[([^\]]+)\]\([^)]+\)`)
	reURL       = regexp.MustCompile(`https?://\S+`)
	reInlineMD  = regexp.MustCompile("[`*_#>-]+")
)

// SpokenFilter converts rich Markdown/engineering agent text into concise TTS-friendly spoken text.
func SpokenFilter(raw string) string {
	hadCode := reCodeFence.MatchString(raw)
	s := reCodeFence.ReplaceAllString(raw, " ")
	s = reMDLink.ReplaceAllString(s, "$1")
	s = reURL.ReplaceAllString(s, "链接")
	s = reInlineMD.ReplaceAllString(s, " ")
	s = strings.Join(strings.Fields(s), " ")
	if hadCode && s == "" {
		return "详细代码和结果已发送到屏幕。"
	}
	if hadCode {
		return s + " 详情已发送到屏幕。"
	}
	return s
}

// SentenceChunker buffers streaming text deltas and emits complete sentences for streaming TTS.
type SentenceChunker struct {
	buf strings.Builder
}

// Push appends delta text and returns any newly completed sentences.
func (c *SentenceChunker) Push(delta string) []string {
	if delta == "" {
		return nil
	}
	c.buf.WriteString(delta)
	var out []string
	for {
		cur := c.buf.String()
		cut := findSentenceCut(cur)
		if cut < 0 {
			break
		}
		seg := strings.TrimSpace(cur[:cut])
		rest := cur[cut:]
		c.buf.Reset()
		c.buf.WriteString(rest)
		if seg != "" {
			out = append(out, seg)
		}
	}
	return out
}

// Flush returns any remaining partial buffered text at end of turn.
func (c *SentenceChunker) Flush() string {
	rem := strings.TrimSpace(c.buf.String())
	c.buf.Reset()
	return rem
}

func findSentenceCut(s string) int {
	for i, r := range s {
		switch r {
		case '。', '！', '？', '.', '!', '?', '\n', '；', ';':
			return i + len(string(r))
		}
	}
	if len(s) >= 180 {
		return len(s)
	}
	return -1
}

// SpeechPipeline coordinates ASR, cloud voiceprint gray-zone review, TTS voice selection, and speaker priority.
type SpeechPipeline struct {
	ASR          ASRProvider
	TTS          TTSProvider
	Reviewer     VoiceReviewer
	Speakers     *SpeakerUsecase
	Arbitrator   *SpeakerArbitrator
	DefaultVoice string
}

// NewSpeechPipeline constructs a SpeechPipeline.
func NewSpeechPipeline(
	asr ASRProvider,
	tts TTSProvider,
	reviewer VoiceReviewer,
	speakers *SpeakerUsecase,
) *SpeechPipeline {
	return &SpeechPipeline{
		ASR:          asr,
		TTS:          tts,
		Reviewer:     reviewer,
		Speakers:     speakers,
		Arbitrator:   NewSpeakerArbitrator(),
		DefaultVoice: "zh_female_calm",
	}
}

// ResolveSpeakerVoice looks up the dynamic TTS voice bound to speakerID, falling back to DefaultVoice.
func (p *SpeechPipeline) ResolveSpeakerVoice(ctx context.Context, speakerID string) string {
	if speakerID != "" && p.Speakers != nil {
		if list, err := p.Speakers.List(ctx, ""); err == nil {
			for _, sp := range list {
				if sp.SpeakerID == speakerID && sp.TTSVoice != "" {
					return sp.TTSVoice
				}
			}
		}
	}
	if p.DefaultVoice != "" {
		return p.DefaultVoice
	}
	return "zh_female_calm"
}
