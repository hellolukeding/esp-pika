package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"

	"esp-pika/backend/gateway/internal/biz"

	"github.com/go-kratos/kratos/v2/log"
)

// MCPServer exposes online devices and fleet topology over Streamable HTTP JSON-RPC 2.0 (`/mcp`).
type MCPServer struct {
	devices  *biz.DeviceUsecase
	speakers *biz.SpeakerUsecase
	log      *log.Helper
}

// NewMCPServer constructs an MCPServer.
func NewMCPServer(devices *biz.DeviceUsecase, speakers *biz.SpeakerUsecase, logger log.Logger) *MCPServer {
	return &MCPServer{
		devices:  devices,
		speakers: speakers,
		log:      log.NewHelper(logger),
	}
}

type jsonRPCRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      any             `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type jsonRPCResponse struct {
	JSONRPC string    `json:"jsonrpc"`
	ID      any       `json:"id"`
	Result  any       `json:"result,omitempty"`
	Error   *rpcError `json:"error,omitempty"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

// ServeHTTP handles MCP JSON-RPC requests.
func (m *MCPServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req jsonRPCRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeRPCError(w, nil, -32700, "parse error")
		return
	}

	ctx := r.Context()
	agentID := r.Header.Get("X-Agent-ID")
	if agentID == "" {
		agentID = "default-agent"
	}
	role := parseRoleHeader(r.Header.Get("X-Speaker-Role"))

	switch req.Method {
	case "initialize":
		var initParams struct {
			ProtocolVersion string `json:"protocolVersion"`
		}
		_ = json.Unmarshal(req.Params, &initParams)
		negotiated := negotiateProtocolVersion(initParams.ProtocolVersion)
		writeRPCResult(w, req.ID, map[string]any{
			"protocolVersion": negotiated,
			"capabilities": map[string]any{
				"tools":     map[string]any{},
				"resources": map[string]any{},
			},
			"serverInfo": map[string]any{
				"name":    "esp-pika-device-mcp",
				"version": "1.0.0",
			},
		})
	case "notifications/initialized", "notifications/cancelled":
		// Streamable HTTP MCP requires 202 Accepted with empty body for JSON-RPC notifications.
		w.WriteHeader(http.StatusAccepted)
	case "ping":
		writeRPCResult(w, req.ID, map[string]any{})
	case "tools/list":
		writeRPCResult(w, req.ID, map[string]any{"tools": m.toolDefinitions()})
	case "tools/call":
		m.handleToolCall(ctx, w, req.ID, agentID, role, req.Params)
	case "resources/list":
		m.handleResourcesList(ctx, w, req.ID)
	case "resources/read":
		m.handleResourcesRead(ctx, w, req.ID, req.Params)
	default:
		writeRPCError(w, req.ID, -32601, fmt.Sprintf("method not found: %s", req.Method))
	}
}

func (m *MCPServer) toolDefinitions() []map[string]any {
	targetProp := map[string]any{
		"type":        "string",
		"description": "Target device MAC (device_id) or room/zone label (e.g. entrance, study)",
	}
	return []map[string]any{
		{
			"name":        "device.camera_capture",
			"description": "Capture a JPEG image from a target ESP32 device camera",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"target":     targetProp,
					"resolution": map[string]any{"type": "string"},
					"question":   map[string]any{"type": "string"},
				},
				"required": []string{"target"},
			},
		},
		{
			"name":        "device.sensor_read",
			"description": "Read a sensor value from a target ESP32 device",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"target":      targetProp,
					"sensor_type": map[string]any{"type": "string"},
				},
				"required": []string{"target", "sensor_type"},
			},
		},
		{
			"name":        "device.actuator_set",
			"description": "Control an actuator (GPIO, relay, LED) on a target ESP32 device",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"target":   targetProp,
					"actuator": map[string]any{"type": "string"},
					"action":   map[string]any{"type": "string"},
					"params":   map[string]any{"type": "object"},
				},
				"required": []string{"target", "actuator", "action"},
			},
		},
		{
			"name":        "device.display_show",
			"description": "Update text or emotion on a target ESP32 LCD/OLED display",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"target":  targetProp,
					"emotion": map[string]any{"type": "string"},
					"text":    map[string]any{"type": "string"},
				},
				"required": []string{"target"},
			},
		},
		{
			"name":        "device.audio_set_volume",
			"description": "Set speaker output volume (0-100) on a target ESP32 device",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"target": targetProp,
					"level":  map[string]any{"type": "integer", "minimum": 0, "maximum": 100},
				},
				"required": []string{"target", "level"},
			},
		},
		{
			"name":        "device.voiceprint_list",
			"description": "List registered voiceprint speakers in a group",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"group_id": map[string]any{"type": "string"},
				},
			},
		},
		{
			"name":        "device.fleet_list",
			"description": "List online and registered ESP32 devices with zone, group, capabilities, and shadow status",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"group_id": map[string]any{"type": "string"},
				},
			},
		},
	}
}

func (m *MCPServer) handleToolCall(ctx context.Context, w http.ResponseWriter, id any, agentID string, role biz.SpeakerRole, rawParams json.RawMessage) {
	var p struct {
		Name      string         `json:"name"`
		Arguments map[string]any `json:"arguments"`
	}
	if err := json.Unmarshal(rawParams, &p); err != nil {
		writeRPCError(w, id, -32602, "invalid params")
		return
	}

	if p.Name == "device.voiceprint_list" {
		groupID, _ := p.Arguments["group_id"].(string)
		speakers, err := m.speakers.List(ctx, groupID)
		if err != nil {
			writeRPCError(w, id, -32000, err.Error())
			return
		}
		b, _ := json.Marshal(speakers)
		writeRPCResult(w, id, map[string]any{
			"content": []map[string]any{{"type": "text", "text": string(b)}},
		})
		return
	}

	if p.Name == "device.fleet_list" {
		groupID, _ := p.Arguments["group_id"].(string)
		body, err := m.devices.FleetTopologyJSONForAgent(ctx, agentID, role, groupID)
		if err != nil {
			if errors.Is(err, biz.ErrACLDenied) {
				writeRPCError(w, id, -32003, err.Error())
				return
			}
			writeRPCResult(w, id, map[string]any{
				"content": []map[string]any{{"type": "text", "text": err.Error()}},
				"isError": true,
			})
			return
		}
		writeRPCResult(w, id, map[string]any{
			"content": []map[string]any{{"type": "text", "text": body}},
		})
		return
	}

	target, _ := p.Arguments["target"].(string)
	if strings.TrimSpace(target) == "" {
		writeRPCError(w, id, -32602, "target is required")
		return
	}

	paramsBytes, _ := json.Marshal(p.Arguments)
	res, err := m.devices.InvokeCommand(ctx, agentID, role, target, &biz.DeviceCommand{
		Capability: p.Name,
		Action:     "invoke",
		ParamsJSON: string(paramsBytes),
		TimeoutMS:  5000,
	})
	if err != nil {
		if errors.Is(err, biz.ErrACLDenied) {
			writeRPCError(w, id, -32003, err.Error())
			return
		}
		writeRPCResult(w, id, map[string]any{
			"content": []map[string]any{{"type": "text", "text": err.Error()}},
			"isError": true,
		})
		return
	}

	outBytes, _ := json.Marshal(res)
	writeRPCResult(w, id, map[string]any{
		"content": []map[string]any{{"type": "text", "text": string(outBytes)}},
		"isError": !res.OK,
	})
}

func (m *MCPServer) handleResourcesList(ctx context.Context, w http.ResponseWriter, id any) {
	resources := []map[string]any{
		{
			"uri":         "fleet://topology",
			"name":        "Fleet Device Topology",
			"description": "All registered ESP32 devices, rooms/zones, capabilities, and online status",
			"mimeType":    "application/json",
		},
	}
	devs, err := m.devices.ListDevices(ctx, "", false)
	if err == nil {
		for _, d := range devs {
			if d.Hello == nil {
				continue
			}
			resources = append(resources,
				map[string]any{
					"uri":      fmt.Sprintf("device://%s/capabilities", d.Hello.DeviceID),
					"name":     fmt.Sprintf("%s capabilities", d.Hello.Zone),
					"mimeType": "application/json",
				},
				map[string]any{
					"uri":      fmt.Sprintf("device://%s/shadow", d.Hello.DeviceID),
					"name":     fmt.Sprintf("%s shadow", d.Hello.Zone),
					"mimeType": "application/json",
				},
			)
		}
	}
	writeRPCResult(w, id, map[string]any{"resources": resources})
}

func (m *MCPServer) handleResourcesRead(ctx context.Context, w http.ResponseWriter, id any, rawParams json.RawMessage) {
	var p struct {
		URI string `json:"uri"`
	}
	if err := json.Unmarshal(rawParams, &p); err != nil {
		writeRPCError(w, id, -32602, "invalid params")
		return
	}

	if p.URI == "fleet://topology" {
		body, err := m.devices.FleetTopologyJSON(ctx, "")
		if err != nil {
			writeRPCError(w, id, -32000, err.Error())
			return
		}
		writeRPCResult(w, id, map[string]any{
			"contents": []map[string]any{{
				"uri":      p.URI,
				"mimeType": "application/json",
				"text":     body,
			}},
		})
		return
	}

	if strings.HasPrefix(p.URI, "device://") {
		trimmed := strings.TrimPrefix(p.URI, "device://")
		parts := strings.SplitN(trimmed, "/", 2)
		if len(parts) == 2 {
			deviceID, kind := parts[0], parts[1]
			switch kind {
			case "shadow":
				sh, err := m.devices.GetShadow(ctx, deviceID)
				if err != nil {
					writeRPCError(w, id, -32004, err.Error())
					return
				}
				b, _ := json.MarshalIndent(sh, "", "  ")
				writeRPCResult(w, id, map[string]any{
					"contents": []map[string]any{{"uri": p.URI, "mimeType": "application/json", "text": string(b)}},
				})
				return
			case "capabilities":
				hello, _, err := m.devices.ResolveTarget(ctx, deviceID)
				if err != nil && hello == nil {
					writeRPCError(w, id, -32004, err.Error())
					return
				}
				b, _ := json.MarshalIndent(hello, "", "  ")
				writeRPCResult(w, id, map[string]any{
					"contents": []map[string]any{{"uri": p.URI, "mimeType": "application/json", "text": string(b)}},
				})
				return
			}
		}
	}

	writeRPCError(w, id, -32004, "resource not found")
}

func negotiateProtocolVersion(requested string) string {
	switch requested {
	case "2025-11-25", "2025-06-18", "2025-03-26":
		return requested
	default:
		return "2025-11-25"
	}
}

func parseRoleHeader(v string) biz.SpeakerRole {
	switch strings.ToLower(strings.TrimSpace(v)) {
	case "owner":
		return biz.SpeakerRoleOwner
	case "member":
		return biz.SpeakerRoleMember
	case "guest":
		return biz.SpeakerRoleGuest
	default:
		return biz.SpeakerRoleGuest
	}
}

func writeRPCResult(w http.ResponseWriter, id any, result any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(jsonRPCResponse{
		JSONRPC: "2.0",
		ID:      id,
		Result:  result,
	})
}

func writeRPCError(w http.ResponseWriter, id any, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(jsonRPCResponse{
		JSONRPC: "2.0",
		ID:      id,
		Error:   &rpcError{Code: code, Message: msg},
	})
}
