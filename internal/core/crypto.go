package core

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"

	"golang.org/x/crypto/pbkdf2"
)

// HashPassword produces a PBKDF2-HMAC-SHA256 encoded string byte-for-byte compatible
// with DirLocker C++ v1.x/v2.x:
//   pbkdf2_sha256$<iterations>$<salt_hex_16bytes>$<dk_hex_32bytes>
func HashPassword(password string, iterations int) (string, error) {
	if iterations <= 0 {
		iterations = DefaultPBKDF2Iter
	}
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return "", fmt.Errorf("failed to generate random salt: %w", err)
	}
	dk := pbkdf2.Key([]byte(password), salt, iterations, 32, sha256.New)
	return fmt.Sprintf("pbkdf2_sha256$%d$%s$%s",
		iterations,
		hex.EncodeToString(salt),
		hex.EncodeToString(dk),
	), nil
}

// VerifyPassword verifies a plaintext password against a stored PBKDF2-HMAC-SHA256 hash.
func VerifyPassword(password, encoded string) bool {
	parts := strings.Split(encoded, "$")
	if len(parts) != 4 || parts[0] != "pbkdf2_sha256" {
		return false
	}
	iterations, err := strconv.Atoi(parts[1])
	if err != nil || iterations < 1000 || iterations > 10000000 {
		return false
	}
	salt, err := hex.DecodeString(parts[2])
	if err != nil || len(salt) == 0 {
		return false
	}
	expectedDK, err := hex.DecodeString(parts[3])
	if err != nil || len(expectedDK) == 0 {
		return false
	}
	actualDK := pbkdf2.Key([]byte(password), salt, iterations, len(expectedDK), sha256.New)
	return subtle.ConstantTimeCompare(actualDK, expectedDK) == 1
}

// GenerateID generates a random 16-byte hex ID (32 lowercase hex chars), matching C++ crypto::generate_id.
func GenerateID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		// Extremely unlikely fallback
		return "00000000000000000000000000000001"
	}
	return hex.EncodeToString(b)
}
