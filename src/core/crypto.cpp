#include "crypto.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

namespace dirlocker {

namespace {

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

constexpr std::array<uint32_t, 64> K256 = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

class Sha256Ctx {
public:
    Sha256Ctx() { reset(); }

    void reset() {
        state_ = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        };
        total_bits_ = 0;
        buf_len_ = 0;
    }

    void update(const uint8_t* data, size_t len) {
        if (len == 0) return;
        total_bits_ += static_cast<uint64_t>(len) * 8ULL;

        size_t offset = 0;
        if (buf_len_ > 0) {
            size_t need = 64 - buf_len_;
            size_t take = std::min(need, len);
            std::memcpy(buf_.data() + buf_len_, data, take);
            buf_len_ += take;
            offset += take;
            if (buf_len_ == 64) {
                compress(buf_.data());
                buf_len_ = 0;
            }
        }
        while (offset + 64 <= len) {
            compress(data + offset);
            offset += 64;
        }
        if (offset < len) {
            buf_len_ = len - offset;
            std::memcpy(buf_.data(), data + offset, buf_len_);
        }
    }

    std::vector<uint8_t> final() {
        buf_[buf_len_++] = 0x80;
        if (buf_len_ > 56) {
            std::memset(buf_.data() + buf_len_, 0, 64 - buf_len_);
            compress(buf_.data());
            buf_len_ = 0;
        }
        std::memset(buf_.data() + buf_len_, 0, 56 - buf_len_);
        for (int i = 0; i < 8; ++i) {
            buf_[56 + i] = static_cast<uint8_t>((total_bits_ >> (56 - i * 8)) & 0xffu);
        }
        compress(buf_.data());

        std::vector<uint8_t> out(32);
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xffu);
            out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xffu);
            out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xffu);
            out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xffu);
        }
        return out;
    }

private:
    void compress(const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
        secure_zero(w, sizeof(w));
    }

    std::array<uint32_t, 8> state_{};
    uint64_t total_bits_ = 0;
    std::array<uint8_t, 64> buf_{};
    size_t buf_len_ = 0;
};

}  // namespace

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    Sha256Ctx ctx;
    ctx.update(data, len);
    return ctx.final();
}

std::vector<uint8_t> sha256(const std::string& data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::vector<uint8_t> hmac_sha256(const uint8_t* key, size_t key_len,
                                 const uint8_t* data, size_t data_len) {
    std::array<uint8_t, 64> k_pad{};
    if (key_len > 64) {
        auto hashed = sha256(key, key_len);
        std::memcpy(k_pad.data(), hashed.data(), hashed.size());
        secure_zero(hashed.data(), hashed.size());
    } else if (key_len > 0) {
        std::memcpy(k_pad.data(), key, key_len);
    }

    std::array<uint8_t, 64> i_key{};
    std::array<uint8_t, 64> o_key{};
    for (size_t i = 0; i < 64; ++i) {
        i_key[i] = k_pad[i] ^ 0x36u;
        o_key[i] = k_pad[i] ^ 0x5cu;
    }

    Sha256Ctx inner;
    inner.update(i_key.data(), 64);
    inner.update(data, data_len);
    auto inner_hash = inner.final();

    Sha256Ctx outer;
    outer.update(o_key.data(), 64);
    outer.update(inner_hash.data(), inner_hash.size());
    auto res = outer.final();

    secure_zero(k_pad.data(), k_pad.size());
    secure_zero(i_key.data(), i_key.size());
    secure_zero(o_key.data(), o_key.size());
    secure_zero(inner_hash.data(), inner_hash.size());
    return res;
}

std::vector<uint8_t> pbkdf2_hmac_sha256(const std::string& password,
                                        const uint8_t* salt, size_t salt_len,
                                        uint32_t iterations, size_t dk_len) {
    if (iterations == 0 || dk_len == 0) return {};

    std::vector<uint8_t> dk;
    dk.reserve(dk_len);

    const uint8_t* pw_ptr = reinterpret_cast<const uint8_t*>(password.data());
    size_t pw_len = password.size();

    uint32_t block_index = 1;
    std::vector<uint8_t> salt_block(salt_len + 4);
    if (salt_len > 0) {
        std::memcpy(salt_block.data(), salt, salt_len);
    }

    while (dk.size() < dk_len) {
        salt_block[salt_len + 0] = static_cast<uint8_t>((block_index >> 24) & 0xffu);
        salt_block[salt_len + 1] = static_cast<uint8_t>((block_index >> 16) & 0xffu);
        salt_block[salt_len + 2] = static_cast<uint8_t>((block_index >> 8) & 0xffu);
        salt_block[salt_len + 3] = static_cast<uint8_t>(block_index & 0xffu);

        auto u = hmac_sha256(pw_ptr, pw_len, salt_block.data(), salt_block.size());
        auto t = u;
        for (uint32_t i = 1; i < iterations; ++i) {
            u = hmac_sha256(pw_ptr, pw_len, u.data(), u.size());
            for (size_t j = 0; j < t.size(); ++j) {
                t[j] ^= u[j];
            }
        }

        size_t take = std::min(t.size(), dk_len - dk.size());
        dk.insert(dk.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
        ++block_index;
    }

    secure_zero(salt_block.data(), salt_block.size());
    return dk;
}

bool random_bytes(uint8_t* out, size_t len) {
    if (len == 0) return true;
    if (!out) return false;
#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
#else
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(out + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        done += static_cast<size_t>(n);
    }
    if (done == len) return true;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    while (done < len) {
        ssize_t n = read(fd, out + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return true;
#endif
}

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return constant_time_equal(reinterpret_cast<const uint8_t*>(a.data()),
                               reinterpret_cast<const uint8_t*>(b.data()),
                               a.size());
}

void secure_zero(void* ptr, size_t len) {
    if (!ptr || len == 0) return;
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
    while (len--) {
        *p++ = 0;
    }
}

void secure_zero(std::string& s) {
    if (!s.empty()) {
        secure_zero(&s[0], s.size());
        s.clear();
    }
}

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = HEX[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = HEX[data[i] & 0xf];
    }
    return out;
}

bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string hash_password(const std::string& password, uint32_t iterations) {
    if (password.empty() || iterations < 1000) return {};
    std::array<uint8_t, 16> salt{};
    if (!random_bytes(salt.data(), salt.size())) return {};

    auto dk = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iterations, 32);
    if (dk.size() != 32) return {};

    std::ostringstream oss;
    oss << "pbkdf2_sha256$" << iterations << "$"
        << bytes_to_hex(salt.data(), salt.size()) << "$"
        << bytes_to_hex(dk.data(), dk.size());
    secure_zero(salt.data(), salt.size());
    secure_zero(dk.data(), dk.size());
    return oss.str();
}

bool verify_password(const std::string& password, const std::string& encoded_hash) {
    if (password.empty() || encoded_hash.empty()) return false;

    std::vector<std::string> parts;
    std::stringstream ss(encoded_hash);
    std::string item;
    while (std::getline(ss, item, '$')) {
        parts.push_back(item);
    }
    if (parts.size() != 4 || parts[0] != "pbkdf2_sha256") return false;

    uint32_t iterations = 0;
    try {
        size_t idx = 0;
        unsigned long val = std::stoul(parts[1], &idx, 10);
        if (idx != parts[1].size() || val < 1000 || val > 5000000) return false;
        iterations = static_cast<uint32_t>(val);
    } catch (...) {
        return false;
    }

    std::vector<uint8_t> salt;
    std::vector<uint8_t> expected_dk;
    if (!hex_to_bytes(parts[2], salt) || !hex_to_bytes(parts[3], expected_dk)) return false;
    if (salt.empty() || expected_dk.size() != 32) return false;

    auto actual_dk = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iterations, 32);
    bool ok = (actual_dk.size() == expected_dk.size()) &&
              constant_time_equal(actual_dk.data(), expected_dk.data(), expected_dk.size());
    secure_zero(actual_dk.data(), actual_dk.size());
    return ok;
}

}  // namespace dirlocker

