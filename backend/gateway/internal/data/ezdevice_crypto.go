package data

import (
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/sha512"
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"math/big"
	"strings"
)

const (
	ezProtocolMajor uint16 = 1
	ezProtocolMinor uint16 = 0

	// Fixed compressed P-256 SPAKE2 points from Device Protocol 1.0 crypto appendix.
	spake2CompressedMHex = "02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"
	spake2CompressedNHex = "03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"
)

var (
	spake2PointMx, spake2PointMy *big.Int
	spake2PointNx, spake2PointNy *big.Int
)

func init() {
	curve := elliptic.P256()
	mBytes, err := hex.DecodeString(spake2CompressedMHex)
	if err != nil {
		panic(fmt.Sprintf("decode SPAKE2 M: %v", err))
	}
	spake2PointMx, spake2PointMy = elliptic.UnmarshalCompressed(curve, mBytes)
	if isP256Infinity(spake2PointMx, spake2PointMy) || !curve.IsOnCurve(spake2PointMx, spake2PointMy) {
		panic("invalid SPAKE2 P-256 point M")
	}

	nBytes, err := hex.DecodeString(spake2CompressedNHex)
	if err != nil {
		panic(fmt.Sprintf("decode SPAKE2 N: %v", err))
	}
	spake2PointNx, spake2PointNy = elliptic.UnmarshalCompressed(curve, nBytes)
	if isP256Infinity(spake2PointNx, spake2PointNy) || !curve.IsOnCurve(spake2PointNx, spake2PointNy) {
		panic("invalid SPAKE2 P-256 point N")
	}
}

// ezCapabilities mirrors Device Protocol 1.0 capability flags on the wire.
type ezCapabilities struct {
	InputText         bool `json:"input_text"`
	InputPCM1616kMono bool `json:"input_pcm16_16k_mono"`
	OutputText        bool `json:"output_text"`
	OutputPCM1616Mono bool `json:"output_pcm16_16k_mono"`
	PlaybackCancel    bool `json:"playback_cancel"`
	DisplayStatus     bool `json:"display_status"`
	DisplayTranscript bool `json:"display_transcript"`
}

func defaultGatewayEZCapabilities() ezCapabilities {
	return ezCapabilities{
		InputText:         true,
		InputPCM1616kMono: false,
		OutputText:        true,
		OutputPCM1616Mono: false,
		PlaybackCancel:    false,
		DisplayStatus:     true,
		DisplayTranscript: true,
	}
}

// CapabilityBytes encodes the 7 capability booleans in exact protocol transcript order.
func (c ezCapabilities) CapabilityBytes() []byte {
	boolByte := func(v bool) byte {
		if v {
			return 0x01
		}
		return 0x00
	}
	return []byte{
		boolByte(c.InputText),
		boolByte(c.InputPCM1616kMono),
		boolByte(c.OutputText),
		boolByte(c.OutputPCM1616Mono),
		boolByte(c.PlaybackCancel),
		boolByte(c.DisplayStatus),
		boolByte(c.DisplayTranscript),
	}
}

func b64uEncode(data []byte) string {
	return base64.RawURLEncoding.EncodeToString(data)
}

func b64uDecodeExact(encoded string, wantLen int) ([]byte, error) {
	if encoded == "" {
		return nil, errors.New("empty base64url string")
	}
	if strings.ContainsAny(encoded, "= \t\r\n+/") {
		return nil, errors.New("non-canonical base64url encoding")
	}
	raw, err := base64.RawURLEncoding.Strict().DecodeString(encoded)
	if err != nil {
		return nil, fmt.Errorf("decode base64url: %w", err)
	}
	if wantLen >= 0 && len(raw) != wantLen {
		return nil, fmt.Errorf("unexpected decoded length %d (want %d)", len(raw), wantLen)
	}
	return raw, nil
}

// encodeAPP encodes parts using U64BE length prefixes per the Device Protocol 1.0 crypto appendix.
func encodeAPP(parts ...[]byte) []byte {
	total := 0
	for _, p := range parts {
		total += 8 + len(p)
	}
	out := make([]byte, 0, total)
	var lenBuf [8]byte
	for _, p := range parts {
		binary.BigEndian.PutUint64(lenBuf[:], uint64(len(p)))
		out = append(out, lenBuf[:]...)
		out = append(out, p...)
	}
	return out
}

// encodeRFC encodes parts using U64LE length prefixes exclusively for SPAKE2 transcript TT.
func encodeRFC(parts ...[]byte) []byte {
	total := 0
	for _, p := range parts {
		total += 8 + len(p)
	}
	out := make([]byte, 0, total)
	var lenBuf [8]byte
	for _, p := range parts {
		binary.LittleEndian.PutUint64(lenBuf[:], uint64(len(p)))
		out = append(out, lenBuf[:]...)
		out = append(out, p...)
	}
	return out
}

func isP256Infinity(x, y *big.Int) bool {
	return x == nil || y == nil || (x.Sign() == 0 && y.Sign() == 0)
}

func i2osp32(v *big.Int) []byte {
	out := make([]byte, 32)
	b := v.Bytes()
	if len(b) > 32 {
		copy(out, b[len(b)-32:])
	} else {
		copy(out[32-len(b):], b)
	}
	return out
}

func hmacSHA256(key, data []byte) [32]byte {
	mac := hmac.New(sha256.New, key)
	_, _ = mac.Write(data)
	var out [32]byte
	copy(out[:], mac.Sum(nil))
	return out
}

func hkdfSHA25632(ikm, info []byte) [32]byte {
	zeroSalt := make([]byte, sha256.Size)
	prk := hmacSHA256(zeroSalt, ikm)
	t1Input := make([]byte, 0, len(info)+1)
	t1Input = append(t1Input, info...)
	t1Input = append(t1Input, 0x01)
	return hmacSHA256(prk[:], t1Input)
}

func derivePasswordScalar(code, requestID, installationID string) (*big.Int, error) {
	if len(code) == 0 || requestID == "" || installationID == "" {
		return nil, errors.New("pairing code, request_id, and installation_id are required")
	}
	pBytes := encodeAPP(
		[]byte("ez-assistant-spake2-password-v1"),
		[]byte(code),
		[]byte(requestID),
		[]byte(installationID),
	)
	digest := sha512.Sum512(pBytes)
	n := elliptic.P256().Params().N
	w := new(big.Int).Mod(new(big.Int).SetBytes(digest[:]), n)
	if w.Sign() <= 0 {
		return nil, errors.New("derived SPAKE2 password scalar is zero")
	}
	return w, nil
}

// buildPairingAssociatedData constructs AD for SPAKE2 confirmation and Ed25519 key binding.
// certFingerprintHex MUST be the 64-character lowercase hex string, and hostNonce MUST be pairing_pake.host_nonce.
func buildPairingAssociatedData(
	requestID string,
	installationID string,
	certFingerprintHex string,
	deviceNonce string,
	hostNonce string,
	caps ezCapabilities,
) []byte {
	return encodeAPP(
		[]byte("ez-assistant-pairing-v1"),
		[]byte(requestID),
		[]byte(installationID),
		[]byte(strings.ToLower(certFingerprintHex)),
		[]byte(deviceNonce),
		[]byte(hostNonce),
		caps.CapabilityBytes(),
	)
}

// spake2PartyA holds ephemeral SPAKE2 P-256 Party A (Device) pairing state.
type spake2PartyA struct {
	requestID      string
	installationID string
	x              *big.Int
	w              *big.Int
	deviceShare    []byte // 65-byte SEC1 uncompressed X
}

func newSPAKE2PartyA(code, requestID, installationID string) (*spake2PartyA, error) {
	nMinusOne := new(big.Int).Sub(elliptic.P256().Params().N, big.NewInt(1))
	r, err := rand.Int(rand.Reader, nMinusOne)
	if err != nil {
		return nil, fmt.Errorf("sample SPAKE2 scalar: %w", err)
	}
	x := new(big.Int).Add(r, big.NewInt(1))
	return newSPAKE2PartyAWithScalar(code, requestID, installationID, x)
}

func newSPAKE2PartyAWithScalar(code, requestID, installationID string, x *big.Int) (*spake2PartyA, error) {
	curve := elliptic.P256()
	n := curve.Params().N
	if x == nil || x.Sign() <= 0 || x.Cmp(n) >= 0 {
		return nil, errors.New("invalid SPAKE2 private scalar x")
	}
	w, err := derivePasswordScalar(code, requestID, installationID)
	if err != nil {
		return nil, err
	}
	xScalar := i2osp32(x)
	wScalar := i2osp32(w)

	xGx, xGy := curve.ScalarBaseMult(xScalar)
	wMx, wMy := curve.ScalarMult(spake2PointMx, spake2PointMy, wScalar)
	xx, xy := curve.Add(xGx, xGy, wMx, wMy)
	if isP256Infinity(xx, xy) || !curve.IsOnCurve(xx, xy) {
		return nil, errors.New("computed SPAKE2 device share is invalid")
	}

	return &spake2PartyA{
		requestID:      requestID,
		installationID: installationID,
		x:              new(big.Int).Set(x),
		w:              w,
		deviceShare:    elliptic.Marshal(curve, xx, xy),
	}, nil
}

func (s *spake2PartyA) DeviceShareB64U() string {
	return b64uEncode(s.deviceShare)
}

func (s *spake2PartyA) Finish(hostShare65 []byte, ad []byte) (sessionKey [16]byte, devConf [32]byte, hostConf [32]byte, err error) {
	curve := elliptic.P256()
	if len(hostShare65) != 65 || hostShare65[0] != 0x04 {
		err = errors.New("invalid host SPAKE2 share format")
		return
	}
	yx, yy := elliptic.Unmarshal(curve, hostShare65)
	if isP256Infinity(yx, yy) || !curve.IsOnCurve(yx, yy) {
		err = errors.New("host SPAKE2 share is not a valid P-256 point")
		return
	}

	wScalar := i2osp32(s.w)
	wNx, wNy := curve.ScalarMult(spake2PointNx, spake2PointNy, wScalar)
	if isP256Infinity(wNx, wNy) {
		err = errors.New("SPAKE2 mask point scaled to infinity")
		return
	}
	negWNy := new(big.Int).Mod(new(big.Int).Sub(curve.Params().P, wNy), curve.Params().P)
	zx, zy := curve.Add(yx, yy, wNx, negWNy)
	if isP256Infinity(zx, zy) || !curve.IsOnCurve(zx, zy) {
		err = errors.New("unmasked host SPAKE2 point is invalid")
		return
	}

	kx, ky := curve.ScalarMult(zx, zy, i2osp32(s.x))
	if isP256Infinity(kx, ky) || !curve.IsOnCurve(kx, ky) {
		err = errors.New("SPAKE2 shared point is invalid")
		return
	}
	k65 := elliptic.Marshal(curve, kx, ky)

	tt := encodeRFC(
		[]byte(s.requestID),
		[]byte(s.installationID),
		s.deviceShare,
		hostShare65,
		k65,
		wScalar,
	)

	schedule := sha256.Sum256(tt)
	copy(sessionKey[:], schedule[:16])
	authKey := schedule[16:32]

	info := make([]byte, 0, len("ConfirmationKeys")+len(ad))
	info = append(info, []byte("ConfirmationKeys")...)
	info = append(info, ad...)
	confKeys := hkdfSHA25632(authKey, info)

	devConf = hmacSHA256(confKeys[:16], tt)
	hostConf = hmacSHA256(confKeys[16:32], tt)
	return sessionKey, devConf, hostConf, nil
}

func buildBindTranscript(ad, publicKeyRaw []byte) []byte {
	return encodeAPP([]byte("ez-assistant-bind-v1"), ad, publicKeyRaw)
}

func computeDeviceBindMAC(sessionKey []byte, bt []byte) [32]byte {
	msg := make([]byte, 0, len("device-bind")+len(bt))
	msg = append(msg, []byte("device-bind")...)
	msg = append(msg, bt...)
	return hmacSHA256(sessionKey, msg)
}

func buildCommitTranscript(bt []byte, deviceID string) []byte {
	return encodeAPP([]byte("ez-assistant-commit-v1"), bt, []byte(deviceID))
}

func computeHostBindAckMAC(sessionKey []byte, ct []byte) [32]byte {
	msg := make([]byte, 0, len("host-bind-ack")+len(ct))
	msg = append(msg, []byte("host-bind-ack")...)
	msg = append(msg, ct...)
	return hmacSHA256(sessionKey, msg)
}

func computeDeviceCommitMAC(sessionKey []byte, ct []byte) [32]byte {
	msg := make([]byte, 0, len("device-commit")+len(ct))
	msg = append(msg, []byte("device-commit")...)
	msg = append(msg, ct...)
	return hmacSHA256(sessionKey, msg)
}

func buildAuthTranscript(
	major uint16,
	minor uint16,
	connectionID string,
	challengeNonce string,
	deviceID string,
	deviceNonce string,
	caps ezCapabilities,
	outputPreference string,
) []byte {
	var majorBuf [2]byte
	var minorBuf [2]byte
	binary.BigEndian.PutUint16(majorBuf[:], major)
	binary.BigEndian.PutUint16(minorBuf[:], minor)
	return encodeAPP(
		[]byte("ez-assistant-auth-v1"),
		majorBuf[:],
		minorBuf[:],
		[]byte(connectionID),
		[]byte(challengeNonce),
		[]byte(deviceID),
		[]byte(deviceNonce),
		caps.CapabilityBytes(),
		[]byte(outputPreference),
	)
}

func signEd25519Transcript(seed32, transcript []byte) ([]byte, error) {
	if len(seed32) != ed25519.SeedSize {
		return nil, fmt.Errorf("invalid Ed25519 seed size %d", len(seed32))
	}
	priv := ed25519.NewKeyFromSeed(seed32)
	return ed25519.Sign(priv, transcript), nil
}

func constantTimeEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	return subtle.ConstantTimeCompare(a, b) == 1
}

func isValidLowerHex(s string, exactLen int) bool {
	if len(s) != exactLen {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}
