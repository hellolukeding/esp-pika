package biz

import (
	"context"
	"fmt"
	"os"
	"strings"
)

const (
	// SettingKeySystemPrompt stores the global Web Console system prompt override.
	SettingKeySystemPrompt = "system_prompt"
	// SettingKeyDeepSeekAPIKey stores the DeepSeek API key configured via the Web Console.
	SettingKeyDeepSeekAPIKey = "deepseek_api_key"
	// SettingKeyDeepSeekModel stores the DeepSeek chat completion model override.
	SettingKeyDeepSeekModel = "deepseek_model"
	// SettingKeyDeepSeekVoice stores the DeepSeek TTS voice override (`stella` or `tide`).
	SettingKeyDeepSeekVoice = "deepseek_voice"

	// DefaultDeepSeekModel is the default DeepSeek chat model when none is persisted.
	DefaultDeepSeekModel = "deepseek-flash"
	// DefaultDeepSeekTTSVoice is the default DeepSeek WS TTS voice when none is persisted.
	DefaultDeepSeekTTSVoice = "stella"

	// DefaultSystemPrompt is the fallback spoken assistant instruction when no override is configured.
	DefaultSystemPrompt = "You are Pika, a concise, helpful smart-home and voice AI assistant running on ESP32-S31 terminals. " +
		"Reply in natural spoken language matching the user's language (avoid markdown tables or raw JSON unless asked). " +
		"When the user asks about connected devices or wants to control hardware (display, speaker volume, sensors, actuators, camera), call the available device MCP tools using the target device_id or zone from the turn context."
)

// DeepSeekSettingsView exposes non-secret DeepSeek runtime status and preferences to the Web Console.
type DeepSeekSettingsView struct {
	Configured bool   `json:"configured"`
	MaskedKey  string `json:"masked_key"`
	Source     string `json:"source"`
	Model      string `json:"model"`
	TTSVoice   string `json:"tts_voice"`
}

// MaskAPIKey redacts a secret key while keeping enough prefix/suffix characters to identify it.
func MaskAPIKey(key string) string {
	trimmed := strings.TrimSpace(key)
	if trimmed == "" {
		return ""
	}
	if len(trimmed) <= 8 {
		return "****"
	}
	return trimmed[:3] + "****" + trimmed[len(trimmed)-4:]
}

// NormalizeDeepSeekVoice maps voice names or legacy aliases onto supported DeepSeek TTS voices (`stella` or `tide`).
func NormalizeDeepSeekVoice(voice, defaultVoice string) string {
	v := strings.ToLower(strings.TrimSpace(voice))
	if v == "" {
		v = strings.ToLower(strings.TrimSpace(defaultVoice))
	}
	switch {
	case v == "stella" || v == "tide":
		return v
	case !strings.Contains(v, "female") && strings.Contains(v, "male"):
		return "tide"
	default:
		return "stella"
	}
}

// SettingsRepo persists gateway runtime configuration key-value pairs.
type SettingsRepo interface {
	Get(ctx context.Context, key string) (string, error)
	Set(ctx context.Context, key, value string) error
}

// SettingsUsecase manages dynamic runtime settings such as global and per-speaker prompts.
type SettingsUsecase struct {
	repo     SettingsRepo
	speakers *SpeakerUsecase
}

// NewSettingsUsecase constructs a SettingsUsecase.
func NewSettingsUsecase(repo SettingsRepo, speakers *SpeakerUsecase) *SettingsUsecase {
	return &SettingsUsecase{
		repo:     repo,
		speakers: speakers,
	}
}

// GetSystemPrompt returns the stored override prompt and the effective prompt (with default fallback).
func (uc *SettingsUsecase) GetSystemPrompt(ctx context.Context) (string, string, error) {
	if uc == nil || uc.repo == nil {
		return "", DefaultSystemPrompt, nil
	}
	stored, err := uc.repo.Get(ctx, SettingKeySystemPrompt)
	if err != nil {
		return "", DefaultSystemPrompt, fmt.Errorf("get system prompt: %w", err)
	}
	stored = strings.TrimSpace(stored)
	effective := stored
	if effective == "" {
		effective = DefaultSystemPrompt
	}
	return stored, effective, nil
}

// SetSystemPrompt saves a new global system prompt override (empty string restores default).
func (uc *SettingsUsecase) SetSystemPrompt(ctx context.Context, prompt string) (string, string, error) {
	stored := strings.TrimSpace(prompt)
	if uc != nil && uc.repo != nil {
		if err := uc.repo.Set(ctx, SettingKeySystemPrompt, stored); err != nil {
			return "", DefaultSystemPrompt, fmt.Errorf("save system prompt: %w", err)
		}
	}
	effective := stored
	if effective == "" {
		effective = DefaultSystemPrompt
	}
	return stored, effective, nil
}

// ResolveTurnPrompt builds the effective system prompt for a turn, merging speaker-specific persona when present.
func (uc *SettingsUsecase) ResolveTurnPrompt(ctx context.Context, speakerID string) string {
	_, effective, err := uc.GetSystemPrompt(ctx)
	if err != nil || strings.TrimSpace(effective) == "" {
		effective = DefaultSystemPrompt
	}
	if speakerID != "" && uc != nil && uc.speakers != nil {
		if list, err := uc.speakers.List(ctx, ""); err == nil {
			for _, sp := range list {
				if sp.SpeakerID == speakerID && strings.TrimSpace(sp.Prompt) != "" {
					effective += "\n\n[Speaker Persona]\n" + strings.TrimSpace(sp.Prompt)
					break
				}
			}
		}
	}
	return effective
}

// GetDeepSeekSettings returns the masked DeepSeek key source plus active model and TTS voice settings.
func (uc *SettingsUsecase) GetDeepSeekSettings(ctx context.Context) (DeepSeekSettingsView, error) {
	view := DeepSeekSettingsView{
		Source:   "none",
		Model:    DefaultDeepSeekModel,
		TTSVoice: DefaultDeepSeekTTSVoice,
	}
	var dbKey string
	if uc != nil && uc.repo != nil {
		var err error
		dbKey, err = uc.repo.Get(ctx, SettingKeyDeepSeekAPIKey)
		if err != nil {
			return view, fmt.Errorf("get deepseek api key: %w", err)
		}
		dbKey = strings.TrimSpace(dbKey)
		if m, err := uc.repo.Get(ctx, SettingKeyDeepSeekModel); err != nil {
			return view, fmt.Errorf("get deepseek model: %w", err)
		} else if trimmed := strings.TrimSpace(m); trimmed != "" {
			view.Model = trimmed
		}
		if v, err := uc.repo.Get(ctx, SettingKeyDeepSeekVoice); err != nil {
			return view, fmt.Errorf("get deepseek voice: %w", err)
		} else if trimmed := strings.TrimSpace(v); trimmed != "" {
			view.TTSVoice = NormalizeDeepSeekVoice(trimmed, DefaultDeepSeekTTSVoice)
		}
	}

	switch {
	case dbKey != "":
		view.Configured = true
		view.Source = "console"
		view.MaskedKey = MaskAPIKey(dbKey)
	default:
		if envKey := strings.TrimSpace(os.Getenv("DEEPSEEK_API_KEY")); envKey != "" {
			view.Configured = true
			view.Source = "env"
			view.MaskedKey = MaskAPIKey(envKey)
		}
	}
	return view, nil
}

// SetDeepSeekSettings persists DeepSeek API key, model, and TTS voice preferences.
func (uc *SettingsUsecase) SetDeepSeekSettings(ctx context.Context, apiKey string, clearKey bool, model, ttsVoice string) (DeepSeekSettingsView, error) {
	if uc != nil && uc.repo != nil {
		if clearKey {
			if err := uc.repo.Set(ctx, SettingKeyDeepSeekAPIKey, ""); err != nil {
				return DeepSeekSettingsView{}, fmt.Errorf("clear deepseek api key: %w", err)
			}
		} else if trimmed := strings.TrimSpace(apiKey); trimmed != "" {
			if err := uc.repo.Set(ctx, SettingKeyDeepSeekAPIKey, trimmed); err != nil {
				return DeepSeekSettingsView{}, fmt.Errorf("save deepseek api key: %w", err)
			}
		}
		if m := strings.TrimSpace(model); m != "" {
			if err := uc.repo.Set(ctx, SettingKeyDeepSeekModel, m); err != nil {
				return DeepSeekSettingsView{}, fmt.Errorf("save deepseek model: %w", err)
			}
		}
		if v := strings.TrimSpace(ttsVoice); v != "" {
			norm := NormalizeDeepSeekVoice(v, DefaultDeepSeekTTSVoice)
			if err := uc.repo.Set(ctx, SettingKeyDeepSeekVoice, norm); err != nil {
				return DeepSeekSettingsView{}, fmt.Errorf("save deepseek voice: %w", err)
			}
		}
	}
	return uc.GetDeepSeekSettings(ctx)
}

// ResolveDeepSeekAPIKey returns the Web Console API key override or falls back to DEEPSEEK_API_KEY.
func (uc *SettingsUsecase) ResolveDeepSeekAPIKey(ctx context.Context) string {
	if uc != nil && uc.repo != nil {
		if stored, err := uc.repo.Get(ctx, SettingKeyDeepSeekAPIKey); err == nil {
			if trimmed := strings.TrimSpace(stored); trimmed != "" {
				return trimmed
			}
		}
	}
	return strings.TrimSpace(os.Getenv("DEEPSEEK_API_KEY"))
}

// ResolveDeepSeekModel returns the configured DeepSeek model override if stored.
func (uc *SettingsUsecase) ResolveDeepSeekModel(ctx context.Context) string {
	if uc != nil && uc.repo != nil {
		if stored, err := uc.repo.Get(ctx, SettingKeyDeepSeekModel); err == nil {
			return strings.TrimSpace(stored)
		}
	}
	return ""
}

// ResolveDeepSeekVoice returns the configured DeepSeek TTS voice override if stored.
func (uc *SettingsUsecase) ResolveDeepSeekVoice(ctx context.Context) string {
	if uc != nil && uc.repo != nil {
		if stored, err := uc.repo.Get(ctx, SettingKeyDeepSeekVoice); err == nil {
			if trimmed := strings.TrimSpace(stored); trimmed != "" {
				return NormalizeDeepSeekVoice(trimmed, DefaultDeepSeekTTSVoice)
			}
		}
	}
	return ""
}
