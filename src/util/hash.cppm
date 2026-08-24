module;

#include <elf.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <openssl/evp.h>

export module sage.util:hash;

import std;

export namespace sage::util {

using std::size_t;

// SHA-256 via OpenSSL EVP -- SHA-NI / AVX2 hardware accelerated.
class Sha256 {
public:
    Sha256() : ctx_(EVP_MD_CTX_new()) {
        if (!ctx_.get() || EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("Cannot initialize OpenSSL SHA-256 context");
        }
    }

    void update(const void* data, size_t len) noexcept {
        if (len == 0 || !ctx_) return;
        (void)EVP_DigestUpdate(ctx_.get(), data, len);
    }

    std::string finalize() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int length = 0;
        if (!ctx_.get() || EVP_DigestFinal_ex(ctx_.get(), digest.data(), &length) != 1) {
            throw std::runtime_error("Cannot finalize OpenSSL SHA-256 digest");
        }
        std::string hex;
        hex.reserve(2 * length);
        static constexpr char hx[] = "0123456789abcdef";
        for (unsigned int i = 0; i < length; ++i) {
            hex.push_back(hx[digest[i] >> 4]);
            hex.push_back(hx[digest[i] & 0xf]);
        }
        return hex;
    }

private:
    struct CtxDeleter {
        void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
    };
    std::unique_ptr<EVP_MD_CTX, CtxDeleter> ctx_;
};

inline std::expected<std::string, std::string> compute_file_sha256(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open file: " + path.string());
    }
    Sha256 hasher;
    char buffer[64 * 1024];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        hasher.update(buffer, static_cast<size_t>(file.gcount()));
    }
    return hasher.finalize();
}

} // namespace sage::util
