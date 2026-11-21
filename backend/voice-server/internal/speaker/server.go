package speaker

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"

	"github.com/coder/websocket"
)

// Server exposes HTTP enroll/identify and WS streaming speaker recognition endpoints.
type Server struct {
	embedder  Embedder
	store     VectorStore
	threshold float32
}

// NewServer constructs a voice-server HTTP/WS handler.
func NewServer(embedder Embedder, store VectorStore, threshold float32) *Server {
	if embedder == nil {
		embedder = &SpectralEmbedder{}
	}
	if threshold <= 0 {
		threshold = 0.6
	}
	return &Server{
		embedder:  embedder,
		store:     store,
		threshold: threshold,
	}
}

// Handler returns the http.Handler mounting all voice-server routes.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
	})
	mux.HandleFunc("/api/v1/speaker/enroll", s.handleEnroll)
	mux.HandleFunc("/api/v1/speaker/identify", s.handleIdentify)
	mux.HandleFunc("/api/v1/speaker/stream", s.handleStream)
	return mux
}

func (s *Server) handleEnroll(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		SpeakerID   string `json:"speaker_id"`
		UID         string `json:"uid"`
		AgentID     string `json:"agent_id"`
		GroupID     string `json:"group_id"`
		Role        int32  `json:"role"`
		AudioBase64 string `json:"audio_base64"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8<<20)).Decode(&req); err != nil || req.SpeakerID == "" {
		http.Error(w, "invalid enroll request", http.StatusBadRequest)
		return
	}
	raw, err := base64.StdEncoding.DecodeString(req.AudioBase64)
	if err != nil || len(raw) == 0 {
		http.Error(w, "invalid audio_base64", http.StatusBadRequest)
		return
	}
	if req.Role == 0 {
		req.Role = 2 // member default
	}
	vec := s.embedder.Extract(raw)
	if err := s.store.Upsert(r.Context(), Record{
		SpeakerID: req.SpeakerID,
		UID:       req.UID,
		AgentID:   req.AgentID,
		GroupID:   req.GroupID,
		Role:      req.Role,
		Vector:    vec,
	}); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ok":         true,
		"speaker_id": req.SpeakerID,
		"dim":        len(vec),
	})
}

func (s *Server) handleIdentify(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, 4<<20))
	if err != nil || len(raw) == 0 {
		http.Error(w, "empty audio payload", http.StatusBadRequest)
		return
	}
	uid := r.URL.Query().Get("uid")
	agentID := r.URL.Query().Get("agent_id")
	vec := s.embedder.Extract(raw)
	hit, err := s.store.Search(r.Context(), vec, uid, agentID, s.threshold)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if hit == nil {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"speaker_id": "", "role": 3, "confidence": 0})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(hit)
}

func (s *Server) handleStream(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	defer func() { _ = conn.CloseNow() }()

	uid := r.URL.Query().Get("uid")
	agentID := r.URL.Query().Get("agent_id")
	var pcmBuf bytes.Buffer

	for {
		mt, data, err := conn.Read(r.Context())
		if err != nil {
			return
		}
		if mt == websocket.MessageBinary {
			pcmBuf.Write(data)
			continue
		}
		var cmd struct {
			Action string `json:"action"`
		}
		if json.Unmarshal(data, &cmd) == nil && cmd.Action == "finish" {
			vec := s.embedder.Extract(pcmBuf.Bytes())
			pcmBuf.Reset()
			hit, err := s.store.Search(r.Context(), vec, uid, agentID, s.threshold)
			var out any = map[string]any{"speaker_id": "", "role": 3, "confidence": 0}
			if err == nil && hit != nil {
				out = hit
			}
			b, _ := json.Marshal(out)
			_ = conn.Write(r.Context(), websocket.MessageText, b)
		}
	}
}
