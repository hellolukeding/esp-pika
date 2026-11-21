package speaker

import (
	"bytes"
	"context"
	"crypto/sha1"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"
)

// Record represents a stored 192-D speaker embedding isolated by (uid, agent_id, group_id).
type Record struct {
	SpeakerID string    `json:"speaker_id"`
	UID       string    `json:"uid"`
	AgentID   string    `json:"agent_id"`
	GroupID   string    `json:"group_id"`
	Role      int32     `json:"role"`
	Vector    []float32 `json:"vector"`
}

// SearchHit returns the closest enrolled speaker above threshold.
type SearchHit struct {
	SpeakerID  string  `json:"speaker_id"`
	Role       int32   `json:"role"`
	Confidence float32 `json:"confidence"`
}

// VectorStore stores and searches 192-D speaker embeddings.
type VectorStore interface {
	Upsert(ctx context.Context, rec Record) error
	Search(ctx context.Context, vec []float32, uid, agentID string, threshold float32) (*SearchHit, error)
}

// QdrantStore persists and searches 192-D Cosine vectors against Qdrant HTTP REST API,
// mirroring updates to an in-process cache for zero-downtime local fallback.
type QdrantStore struct {
	baseURL    string
	collection string
	client     *http.Client

	mu       sync.RWMutex
	fallback map[string]Record
}

// NewQdrantStore initializes a Qdrant client targeting baseURL (e.g. http://127.0.0.1:6333).
func NewQdrantStore(baseURL, collection string) *QdrantStore {
	if collection == "" {
		collection = "speaker_embeddings"
	}
	return &QdrantStore{
		baseURL:    baseURL,
		collection: collection,
		client:     &http.Client{Timeout: 5 * time.Second},
		fallback:   make(map[string]Record),
	}
}

// EnsureCollection idempotently creates the 192-D Cosine collection in Qdrant.
func (q *QdrantStore) EnsureCollection(ctx context.Context) error {
	if q.baseURL == "" {
		return nil
	}
	body, _ := json.Marshal(map[string]any{
		"vectors": map[string]any{
			"size":     EmbeddingDim,
			"distance": "Cosine",
		},
	})
	url := fmt.Sprintf("%s/collections/%s", q.baseURL, q.collection)
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := q.client.Do(req)
	if err != nil {
		return err
	}
	_ = resp.Body.Close()
	if resp.StatusCode == http.StatusOK || resp.StatusCode == http.StatusCreated || resp.StatusCode == http.StatusConflict {
		return nil
	}
	return fmt.Errorf("qdrant create collection HTTP %d", resp.StatusCode)
}

// Upsert stores or updates a speaker's 192-D embedding in Qdrant and the fallback map.
func (q *QdrantStore) Upsert(ctx context.Context, rec Record) error {
	q.mu.Lock()
	q.fallback[rec.SpeakerID] = rec
	q.mu.Unlock()

	if q.baseURL == "" {
		return nil
	}
	_ = q.EnsureCollection(ctx)

	pointID := stablePointID(rec.SpeakerID, rec.UID, rec.AgentID)
	body, _ := json.Marshal(map[string]any{
		"points": []map[string]any{
			{
				"id":     pointID,
				"vector": rec.Vector,
				"payload": map[string]any{
					"speaker_id": rec.SpeakerID,
					"uid":        rec.UID,
					"agent_id":   rec.AgentID,
					"group_id":   rec.GroupID,
					"role":       rec.Role,
				},
			},
		},
	})
	url := fmt.Sprintf("%s/collections/%s/points?wait=true", q.baseURL, q.collection)
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := q.client.Do(req)
	if err != nil {
		return err
	}
	_ = resp.Body.Close()
	if resp.StatusCode >= 300 {
		return fmt.Errorf("qdrant upsert HTTP %d", resp.StatusCode)
	}
	return nil
}

// Search queries Qdrant for the highest-scoring speaker isolated by (uid, agent_id).
func (q *QdrantStore) Search(ctx context.Context, vec []float32, uid, agentID string, threshold float32) (*SearchHit, error) {
	if q.baseURL != "" {
		hit, err := q.searchQdrant(ctx, vec, uid, agentID, threshold)
		if err == nil {
			return hit, nil
		}
	}
	return q.searchFallback(vec, uid, agentID, threshold), nil
}

func (q *QdrantStore) searchQdrant(ctx context.Context, vec []float32, uid, agentID string, threshold float32) (*SearchHit, error) {
	var must []map[string]any
	if uid != "" {
		must = append(must, map[string]any{"key": "uid", "match": map[string]any{"value": uid}})
	}
	if agentID != "" {
		must = append(must, map[string]any{"key": "agent_id", "match": map[string]any{"value": agentID}})
	}
	reqMap := map[string]any{
		"vector":          vec,
		"limit":           1,
		"with_payload":    true,
		"score_threshold": threshold,
	}
	if len(must) > 0 {
		reqMap["filter"] = map[string]any{"must": must}
	}
	body, _ := json.Marshal(reqMap)
	url := fmt.Sprintf("%s/collections/%s/points/search", q.baseURL, q.collection)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := q.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode >= 300 {
		return nil, fmt.Errorf("qdrant search HTTP %d", resp.StatusCode)
	}

	var out struct {
		Result []struct {
			Score   float32 `json:"score"`
			Payload struct {
				SpeakerID string `json:"speaker_id"`
				Role      int32  `json:"role"`
			} `json:"payload"`
		} `json:"result"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, err
	}
	if len(out.Result) == 0 {
		return nil, nil
	}
	top := out.Result[0]
	return &SearchHit{
		SpeakerID:  top.Payload.SpeakerID,
		Role:       top.Payload.Role,
		Confidence: top.Score,
	}, nil
}

func (q *QdrantStore) searchFallback(vec []float32, uid, agentID string, threshold float32) *SearchHit {
	q.mu.RLock()
	defer q.mu.RUnlock()
	var best *SearchHit
	bestScore := threshold
	for _, rec := range q.fallback {
		if uid != "" && rec.UID != uid {
			continue
		}
		if agentID != "" && rec.AgentID != agentID {
			continue
		}
		score := CosineSimilarity(vec, rec.Vector)
		if score >= bestScore {
			bestScore = score
			best = &SearchHit{
				SpeakerID:  rec.SpeakerID,
				Role:       rec.Role,
				Confidence: score,
			}
		}
	}
	return best
}

func stablePointID(speakerID, uid, agentID string) uint64 {
	h := sha1.Sum([]byte(speakerID + "|" + uid + "|" + agentID))
	return binary.BigEndian.Uint64(h[:8])
}
