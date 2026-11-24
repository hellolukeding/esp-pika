package data

import (
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"math/big"
	"os"
	"path/filepath"
	"testing"
)

type upstreamCryptoFixture struct {
	Pairing struct {
		PairingCode            string         `json:"pairing_code"`
		PairingRequestID       string         `json:"pairing_request_id"`
		InstallationID         string         `json:"installation_id"`
		CertificateFingerprint string         `json:"certificate_fingerprint"`
		DeviceNonce            string         `json:"device_nonce"`
		HostNonce              string         `json:"host_nonce"`
		Capabilities           ezCapabilities `json:"capabilities"`
		DeviceScalarHex        string         `json:"device_scalar_hex"`
		HostScalarHex          string         `json:"host_scalar_hex"`
		AssociatedData         string         `json:"associated_data"`
		DeviceShare            string         `json:"device_share"`
		HostShare              string         `json:"host_share"`
		DeviceConfirmation     string         `json:"device_confirmation"`
		HostConfirmation       string         `json:"host_confirmation"`
		SessionKey             string         `json:"session_key"`
	} `json:"pairing"`
	Authentication struct {
		ConnectionID     string `json:"connection_id"`
		HostNonce        string `json:"host_nonce"`
		DeviceID         string `json:"device_id"`
		DeviceNonce      string `json:"device_nonce"`
		OutputPreference string `json:"output_preference"`
		Transcript       string `json:"transcript"`
		PrivateSeed      string `json:"private_seed"`
		PublicKey        string `json:"public_key"`
		Signature        string `json:"signature"`
	} `json:"authentication"`
}

func TestEZDeviceCryptoUpstreamVectors(t *testing.T) {
	// Frozen values from upstream agent-robot/firmware/test/upstream/device-protocol-v1/crypto-v1.json
	const embeddedFixture = `{
  "pairing": {
    "pairing_code": "123456",
    "pairing_request_id": "fixture-request",
    "installation_id": "0123456789abcdef0123456789abcdef",
    "certificate_fingerprint": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "device_nonce": "device-nonce-fixture",
    "host_nonce": "host-nonce-fixture",
    "capabilities": {
      "input_text": true,
      "input_pcm16_16k_mono": false,
      "output_text": true,
      "output_pcm16_16k_mono": false,
      "playback_cancel": false,
      "display_status": true,
      "display_transcript": true
    },
    "device_scalar_hex": "0000000000000000000000000000000000000000000000000000000000000005",
    "host_scalar_hex": "0000000000000000000000000000000000000000000000000000000000000007",
    "associated_data": "AAAAAAAAABdlei1hc3Npc3RhbnQtcGFpcmluZy12MQAAAAAAAAAPZml4dHVyZS1yZXF1ZXN0AAAAAAAAACAwMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZgAAAAAAAABAYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYQAAAAAAAAAUZGV2aWNlLW5vbmNlLWZpeHR1cmUAAAAAAAAAEmhvc3Qtbm9uY2UtZml4dHVyZQAAAAAAAAAHAQABAAABAQ",
    "device_share": "BGskAGlnvzUJBxcVmuSzarCd1BAgqe7rMZATrcEgL4soKJC4kBfoQzY7YkROnOZ2v40xIQkkWqs09tEJolZ3xFg",
    "host_share": "BJOVw9GEr8DMp481poxTdlcGp34O0wULhrujTilqHd1kuS2zRLKHpybBW3JytcWHGDfIVjI8fT1iHXEBga5NQOY",
    "device_confirmation": "bafuqgquWlAGDOBpP9Yb0Z_lkabRwcuYuHZt0-SBaS8",
    "host_confirmation": "4lS4w6y0TnLOX0jJZqGZZ4ZJj06bRcI6eNMcO4p2grU",
    "session_key": "dB77FpNwOr2EXcc6xpTh3A"
  },
  "authentication": {
    "connection_id": "connection-fixture",
    "host_nonce": "host-auth-nonce",
    "device_id": "device-fixture",
    "device_nonce": "device-auth-nonce",
    "output_preference": "text",
    "transcript": "AAAAAAAAABRlei1hc3Npc3RhbnQtYXV0aC12MQAAAAAAAAACAAEAAAAAAAAAAgAAAAAAAAAAABJjb25uZWN0aW9uLWZpeHR1cmUAAAAAAAAAD2hvc3QtYXV0aC1ub25jZQAAAAAAAAAOZGV2aWNlLWZpeHR1cmUAAAAAAAAAEWRldmljZS1hdXRoLW5vbmNlAAAAAAAAAAcBAAEAAAEBAAAAAAAAAAR0ZXh0",
    "private_seed": "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",
    "public_key": "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg",
    "signature": "0Yn3d21SvbS0GEj9BLydIypy4JUotUiburl5jojJTUo9JCXCJ7S9k7VKWM-ZnRTnDBxf7whvbntuUyDraAFsDg"
  }
}`

	raw := []byte(embeddedFixture)
	upstreamPath := filepath.Join("..", "..", "..", "..", "agent-robot", "firmware", "test", "upstream", "device-protocol-v1", "crypto-v1.json")
	if diskRaw, err := os.ReadFile(upstreamPath); err == nil {
		raw = diskRaw
	}

	var fix upstreamCryptoFixture
	if err := json.Unmarshal(raw, &fix); err != nil {
		t.Fatalf("unmarshal fixture: %v", err)
	}

	ad := buildPairingAssociatedData(
		fix.Pairing.PairingRequestID,
		fix.Pairing.InstallationID,
		fix.Pairing.CertificateFingerprint,
		fix.Pairing.DeviceNonce,
		fix.Pairing.HostNonce,
		fix.Pairing.Capabilities,
	)
	if got := b64uEncode(ad); got != fix.Pairing.AssociatedData {
		t.Fatalf("AD mismatch:\n got: %s\nwant: %s", got, fix.Pairing.AssociatedData)
	}

	scalarBytes, err := hex.DecodeString(fix.Pairing.DeviceScalarHex)
	if err != nil {
		t.Fatalf("decode device scalar: %v", err)
	}
	devScalar := new(big.Int).SetBytes(scalarBytes)
	partyA, err := newSPAKE2PartyAWithScalar(
		fix.Pairing.PairingCode,
		fix.Pairing.PairingRequestID,
		fix.Pairing.InstallationID,
		devScalar,
	)
	if err != nil {
		t.Fatalf("newSPAKE2PartyAWithScalar: %v", err)
	}
	if got := partyA.DeviceShareB64U(); got != fix.Pairing.DeviceShare {
		t.Fatalf("device_share mismatch:\n got: %s\nwant: %s", got, fix.Pairing.DeviceShare)
	}

	hostShare, err := b64uDecodeExact(fix.Pairing.HostShare, 65)
	if err != nil {
		t.Fatalf("decode host share: %v", err)
	}
	sessKey, devConf, hostConf, err := partyA.Finish(hostShare, ad)
	if err != nil {
		t.Fatalf("SPAKE2 Finish: %v", err)
	}
	if got := b64uEncode(devConf[:]); got != fix.Pairing.DeviceConfirmation {
		t.Fatalf("device_confirmation mismatch:\n got: %s\nwant: %s", got, fix.Pairing.DeviceConfirmation)
	}
	if got := b64uEncode(hostConf[:]); got != fix.Pairing.HostConfirmation {
		t.Fatalf("host_confirmation mismatch:\n got: %s\nwant: %s", got, fix.Pairing.HostConfirmation)
	}
	if got := b64uEncode(sessKey[:]); got != fix.Pairing.SessionKey {
		t.Fatalf("session_key mismatch:\n got: %s\nwant: %s", got, fix.Pairing.SessionKey)
	}

	authTranscript := buildAuthTranscript(
		ezProtocolMajor,
		ezProtocolMinor,
		fix.Authentication.ConnectionID,
		fix.Authentication.HostNonce,
		fix.Authentication.DeviceID,
		fix.Authentication.DeviceNonce,
		fix.Pairing.Capabilities,
		fix.Authentication.OutputPreference,
	)
	if got := b64uEncode(authTranscript); got != fix.Authentication.Transcript {
		t.Fatalf("auth transcript mismatch:\n got: %s\nwant: %s", got, fix.Authentication.Transcript)
	}

	seed, err := b64uDecodeExact(fix.Authentication.PrivateSeed, ed25519.SeedSize)
	if err != nil {
		t.Fatalf("decode seed: %v", err)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	pub := priv.Public().(ed25519.PublicKey)
	if got := b64uEncode(pub); got != fix.Authentication.PublicKey {
		t.Fatalf("public_key mismatch:\n got: %s\nwant: %s", got, fix.Authentication.PublicKey)
	}
	sig, err := signEd25519Transcript(seed, authTranscript)
	if err != nil {
		t.Fatalf("sign AUTH: %v", err)
	}
	if got := b64uEncode(sig); got != fix.Authentication.Signature {
		t.Fatalf("signature mismatch:\n got: %s\nwant: %s", got, fix.Authentication.Signature)
	}
	if !ed25519.Verify(pub, authTranscript, sig) {
		t.Fatal("ed25519 verify failed on fixture transcript")
	}

	// Bind / Commit transcript negative checks
	bt := buildBindTranscript(ad, pub)
	bindMAC := computeDeviceBindMAC(sessKey[:], bt)
	ct := buildCommitTranscript(bt, fix.Authentication.DeviceID)
	ackMAC := computeHostBindAckMAC(sessKey[:], ct)
	commitMAC := computeDeviceCommitMAC(sessKey[:], ct)
	if constantTimeEqual(bindMAC[:], commitMAC[:]) || constantTimeEqual(ackMAC[:], commitMAC[:]) {
		t.Fatal("distinct bind/ack/commit MAC labels collided")
	}
	if ed25519.Verify(pub, append(bt, 'x'), sig) {
		t.Fatal("tampered transcript unexpectedly verified")
	}
}

func TestEZDeviceCryptoNegativeCases(t *testing.T) {
	if _, err := b64uDecodeExact("YWJj=", 3); err == nil {
		t.Fatal("expected error on padded base64url")
	}
	if _, err := b64uDecodeExact("YWJj", 4); err == nil {
		t.Fatal("expected error on wrong decoded length")
	}

	partyA, err := newSPAKE2PartyA("654321", "req-1", "0123456789abcdef0123456789abcdef")
	if err != nil {
		t.Fatalf("newSPAKE2PartyA: %v", err)
	}
	badPoint := make([]byte, 65)
	badPoint[0] = 0x04
	if _, _, _, err := partyA.Finish(badPoint, []byte("ad")); err == nil {
		t.Fatal("expected invalid P-256 point error")
	}
}
