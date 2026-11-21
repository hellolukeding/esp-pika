package ezruntime

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// SoftwareVersion matches ez-assistant Cargo.toml 0.33.1 compatibility contract.
	SoftwareVersion = "0.33.1"
	headerClientVer = "x-ez-client-version"
	headerMinVer    = "x-ez-min-compatible-version"
)

// Discovery mirrors `<runtime_home>/run/runtime.json` written by ez-assistant-runtime.
type Discovery struct {
	Address     string `json:"address"`
	InstanceID  string `json:"instance_id"`
	AccessToken string `json:"access_token"`
	PID         uint32 `json:"pid"`
}

// Config configures how ez-bridge connects to ez-assistant-runtime.
type Config struct {
	BaseURL     string
	AccessToken string
	RuntimeHome string
	HTTPClient  *http.Client
}

// EventEnvelope matches assistant-protocol RuntimeEventEnvelope emitted over GET /events.
type EventEnvelope struct {
	Sequence    uint64       `json:"sequence"`
	EmittedAtMS int64        `json:"emitted_at_ms"`
	Event       RuntimeEvent `json:"event"`
}

// RuntimeEvent captures the subset of assistant-protocol RuntimeEvent variants consumed by esp-pika.
type RuntimeEvent struct {
	Type      string          `json:"type"`
	SessionID string          `json:"session_id,omitempty"`
	RunID     string          `json:"run_id,omitempty"`
	Step      uint32          `json:"step,omitempty"`
	PartID    string          `json:"part_id,omitempty"`
	Delta     string          `json:"delta,omitempty"`
	CallID    string          `json:"call_id,omitempty"`
	ToolName  string          `json:"tool_name,omitempty"`
	Status    string          `json:"status,omitempty"`
	Error     *ErrorInfo      `json:"error,omitempty"`
	Approval  *ApprovalDetail `json:"approval,omitempty"`
}

// ErrorInfo matches assistant-protocol RuntimeErrorInfo.
type ErrorInfo struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// ApprovalDetail captures ApprovalRequested fields needed for auto-decision.
type ApprovalDetail struct {
	ApprovalID         string   `json:"approval_id"`
	SessionID          string   `json:"session_id"`
	RunID              string   `json:"run_id"`
	Subject            string   `json:"subject"`
	AvailableDecisions []string `json:"available_decisions"`
}

// Client manages authenticated HTTP commands and SSE event streaming with ez-assistant-runtime.
type Client struct {
	baseURL     string
	token       string
	runtimeHome string
	httpClient  *http.Client
	reqSeq      atomic.Uint64

	subMu       sync.RWMutex
	subscribers map[string]chan RuntimeEvent // keyed by ez session_id
	nextSubID   atomic.Uint64

	sseMu     sync.Mutex
	sseCancel context.CancelFunc
	sseDone   chan struct{}
}

// LoadDiscovery reads `<runtime_home>/run/runtime.json`.
func LoadDiscovery(runtimeHome string) (*Discovery, error) {
	if runtimeHome == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return nil, fmt.Errorf("resolve user home: %w", err)
		}
		runtimeHome = filepath.Join(home, ".ez-assistant")
	}
	p := filepath.Join(runtimeHome, "run", "runtime.json")
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, fmt.Errorf("read ez-assistant discovery %s: %w", p, err)
	}
	var d Discovery
	if err := json.Unmarshal(b, &d); err != nil {
		return nil, fmt.Errorf("parse discovery %s: %w", p, err)
	}
	if d.Address == "" || d.AccessToken == "" {
		return nil, fmt.Errorf("incomplete discovery in %s", p)
	}
	return &d, nil
}

// NewClient constructs a Client from explicit endpoint credentials or `<runtime_home>/run/runtime.json`.
func NewClient(cfg Config) (*Client, error) {
	baseURL := strings.TrimRight(cfg.BaseURL, "/")
	token := cfg.AccessToken
	if baseURL == "" || token == "" {
		d, err := LoadDiscovery(cfg.RuntimeHome)
		if err != nil {
			return nil, err
		}
		if baseURL == "" {
			addr := d.Address
			if !strings.HasPrefix(addr, "http://") && !strings.HasPrefix(addr, "https://") {
				addr = "http://" + addr
			}
			baseURL = strings.TrimRight(addr, "/")
		}
		if token == "" {
			token = d.AccessToken
		}
	}
	hc := cfg.HTTPClient
	if hc == nil {
		hc = http.DefaultClient
	}
	return &Client{
		baseURL:     baseURL,
		token:       token,
		runtimeHome: cfg.RuntimeHome,
		httpClient:  hc,
		subscribers: make(map[string]chan RuntimeEvent),
	}, nil
}

// StartEventStream launches the SSE reader for `GET /events`. Call StopEventStream to shut down cleanly.
func (c *Client) StartEventStream(ctx context.Context) {
	c.sseMu.Lock()
	defer c.sseMu.Unlock()
	if c.sseCancel != nil {
		return
	}
	streamCtx, cancel := context.WithCancel(ctx)
	done := make(chan struct{})
	c.sseCancel = cancel
	c.sseDone = done

	go func() {
		defer close(done)
		c.runSSELoop(streamCtx)
	}()
}

// StopEventStream stops the background SSE loop and waits for completion.
func (c *Client) StopEventStream() {
	c.sseMu.Lock()
	cancel := c.sseCancel
	done := c.sseDone
	c.sseCancel = nil
	c.sseDone = nil
	c.sseMu.Unlock()

	if cancel != nil {
		cancel()
	}
	if done != nil {
		<-done
	}
}

// SubscribeSession returns a buffered event channel scoped to `sessionID` and an unsubscribe cleanup func.
func (c *Client) SubscribeSession(sessionID string) (<-chan RuntimeEvent, func()) {
	id := fmt.Sprintf("%s#%d", sessionID, c.nextSubID.Add(1))
	ch := make(chan RuntimeEvent, 128)
	c.subMu.Lock()
	c.subscribers[id] = ch
	c.subMu.Unlock()
	return ch, func() {
		c.subMu.Lock()
		delete(c.subscribers, id)
		close(ch)
		c.subMu.Unlock()
	}
}

func (c *Client) dispatchEvent(evt RuntimeEvent) {
	sessionID := evt.SessionID
	if sessionID == "" && evt.Approval != nil {
		sessionID = evt.Approval.SessionID
	}
	if sessionID == "" {
		return
	}
	prefix := sessionID + "#"
	c.subMu.RLock()
	defer c.subMu.RUnlock()
	for k, ch := range c.subscribers {
		if strings.HasPrefix(k, prefix) {
			select {
			case ch <- evt:
			default:
			}
		}
	}
}

func (c *Client) runSSELoop(ctx context.Context) {
	for ctx.Err() == nil {
		c.connectAndStreamSSE(ctx)
		if ctx.Err() != nil {
			return
		}
		timer := time.NewTimer(300 * time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}

func (c *Client) connectAndStreamSSE(ctx context.Context) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/events", nil)
	if err != nil {
		return
	}
	c.applyHeaders(req)
	req.Header.Set("Accept", "text/event-stream")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return
	}

	scanner := bufio.NewScanner(resp.Body)
	scanner.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)

	var eventName string
	var dataLines []string
	flush := func() {
		if len(dataLines) == 0 {
			eventName = ""
			return
		}
		raw := strings.Join(dataLines, "\n")
		dataLines = nil
		if eventName == "runtime_event" || eventName == "" {
			var env EventEnvelope
			if err := json.Unmarshal([]byte(raw), &env); err == nil && env.Event.Type != "" {
				c.dispatchEvent(env.Event)
			}
		}
		eventName = ""
	}

	for scanner.Scan() {
		line := scanner.Text()
		if line == "" {
			flush()
			continue
		}
		if strings.HasPrefix(line, ":") {
			continue
		}
		if strings.HasPrefix(line, "event:") {
			eventName = strings.TrimSpace(strings.TrimPrefix(line, "event:"))
		} else if strings.HasPrefix(line, "data:") {
			dataLines = append(dataLines, strings.TrimSpace(strings.TrimPrefix(line, "data:")))
		}
	}
	flush()
}

// Health verifies connectivity with GET /health.
func (c *Client) Health(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/health", nil)
	if err != nil {
		return err
	}
	c.applyHeaders(req)
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	_ = resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("ez-assistant health status %d", resp.StatusCode)
	}
	return nil
}

// EnsureReady calls POST /runtime/ensure-ready so user services and MCP registry are booted.
func (c *Client) EnsureReady(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/runtime/ensure-ready", nil)
	if err != nil {
		return err
	}
	c.applyHeaders(req)
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	_ = resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("ez-assistant ensure-ready status %d", resp.StatusCode)
	}
	return nil
}

// CreateSession calls RuntimeCommand::CreateSession and returns the new ez session_id.
func (c *Client) CreateSession(ctx context.Context, title string) (string, error) {
	var out struct {
		Session struct {
			SessionID string `json:"session_id"`
		} `json:"session"`
	}
	payload := map[string]any{
		"title":           title,
		"model_selection": nil,
	}
	if err := c.execRuntimeCommand(ctx, "create_session", payload, &out); err != nil {
		return "", err
	}
	if out.Session.SessionID == "" {
		return "", fmt.Errorf("create_session returned empty session_id")
	}
	return out.Session.SessionID, nil
}

// SetSessionApprovalMode calls RuntimeCommand::SetSessionApprovalMode (`ask` or `auto`).
func (c *Client) SetSessionApprovalMode(ctx context.Context, sessionID, mode string) error {
	return c.execRuntimeCommand(ctx, "set_session_approval_mode", map[string]any{
		"session_id":    sessionID,
		"approval_mode": mode,
	}, nil)
}

// DecideApproval resolves an ApprovalRequested prompt (`allow_once`, `allow_session`, `deny`).
func (c *Client) DecideApproval(ctx context.Context, sessionID, approvalID, decision string) error {
	return c.execRuntimeCommand(ctx, "decide_approval", map[string]any{
		"session_id":  sessionID,
		"approval_id": approvalID,
		"decision":    decision,
	}, nil)
}

// SubmitInput submits a user turn to `sessionID` and returns `(input_id, run_id)`.
func (c *Client) SubmitInput(ctx context.Context, sessionID, message, mcpServerKey, idempotencyKey string) (string, string, error) {
	payload := map[string]any{
		"session_id": sessionID,
		"message":    message,
		"variant":    "build",
	}
	if mcpServerKey != "" {
		payload["mcp_server_key"] = mcpServerKey
	}
	if idempotencyKey != "" {
		payload["idempotency_key"] = idempotencyKey
	}
	var out struct {
		InputID string `json:"input_id"`
		Run     *struct {
			RunID string `json:"run_id"`
		} `json:"run"`
	}
	if err := c.execRuntimeCommand(ctx, "submit_input", payload, &out); err != nil {
		return "", "", err
	}
	runID := ""
	if out.Run != nil {
		runID = out.Run.RunID
	}
	return out.InputID, runID, nil
}

// CancelRun cancels an active run in `sessionID`.
func (c *Client) CancelRun(ctx context.Context, sessionID, runID string) error {
	return c.execRuntimeCommand(ctx, "cancel_run", map[string]any{
		"session_id": sessionID,
		"run_id":     runID,
	}, nil)
}

func (c *Client) execRuntimeCommand(ctx context.Context, cmdType string, payload any, out any) error {
	reqBody := map[string]any{
		"request_id": fmt.Sprintf("esp-%d", c.reqSeq.Add(1)),
		"command": map[string]any{
			"scope": "runtime",
			"payload": map[string]any{
				"type":    cmdType,
				"payload": payload,
			},
		},
	}
	raw, err := json.Marshal(reqBody)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/commands", bytes.NewReader(raw))
	if err != nil {
		return err
	}
	c.applyHeaders(req)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer func() { _ = resp.Body.Close() }()

	respBytes, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("ez-assistant command %s HTTP %d: %s", cmdType, resp.StatusCode, string(respBytes))
	}

	var env struct {
		Result struct {
			Scope   string `json:"scope"`
			Payload struct {
				Type    string          `json:"type"`
				Payload json.RawMessage `json:"payload"`
			} `json:"payload"`
		} `json:"result"`
		Error *ErrorInfo `json:"error"`
	}
	if err := json.Unmarshal(respBytes, &env); err != nil {
		return fmt.Errorf("decode command %s response: %w", cmdType, err)
	}
	if env.Error != nil {
		return fmt.Errorf("ez-assistant error (%s): %s", env.Error.Code, env.Error.Message)
	}
	if out != nil && len(env.Result.Payload.Payload) > 0 {
		if err := json.Unmarshal(env.Result.Payload.Payload, out); err != nil {
			return fmt.Errorf("decode command %s payload: %w", cmdType, err)
		}
	}
	return nil
}

func (c *Client) applyHeaders(req *http.Request) {
	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}
	req.Header.Set(headerClientVer, SoftwareVersion)
	req.Header.Set(headerMinVer, SoftwareVersion)
}
