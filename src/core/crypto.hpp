#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dirlocker {

// Computes raw SHA-256 digest (32 bytes).
std::vector<uint8_t> sha256(const uint8_t* data, size_t len);
std::vector<uint8_t> sha256(const std::string& data);

// Computes HMAC-SHA256 (32 bytes).
std::vector<uint8_t> hmac_sha256(const uint8_t* key, size_t key_len,
                                 const uint8_t* data, size_t data_len);

// Computes PBKDF2-HMAC-SHA256 derived key of length dk_len.
std::vector<uint8_t> pbkdf2_hmac_sha256(const std::string& password,
                                        const uint8_t* salt, size_t salt_len,
                                        uint32_t iterations, size_t dk_len);

// Generates cryptographically secure random bytes.
bool random_bytes(uint8_t* out, size_t len);

// Constant-time equality comparison.
bool constant_time_equal(const std::string& a, const std::string& b);
bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len);

// Securely zeroes memory so compiler won't optimize it away.
void secure_zero(void* ptr, size_t len);
void secure_zero(std::string& s);

// Creates encoded PBKDF2 hash string: "pbkdf2_sha256$<iters>$<salt_hex>$<dk_hex>"
// Returns empty string on CSPRNG failure.
std::string hash_password(const std::string& password, uint32_t iterations = 60000);

// Verifies a password against an encoded hash string.
bool verify_password(const std::string& password, const std::string& encoded_hash);

std::string bytes_to_hex(const uint8_t* data, size_t len);
bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out);

}  // namespace dirlocker

