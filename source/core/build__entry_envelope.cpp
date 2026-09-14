#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

static std::vector<std::uint8_t> ReadFile(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input.good())
        return {};
    return bytes;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: build_kirkware_entry_envelope.exe <payload-image> <handoff-blob> <output>\n");
        return 2;
    }
    const auto image = ReadFile(argv[1]);
    const auto handoff = ReadFile(argv[2]);
    if (image.size() < 0x682D40 || handoff.size() != 0x30240) {
        std::fprintf(stderr, "invalid payload image or handoff blob\n");
        return 1;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE static_key = nullptr;
    BCRYPT_KEY_HANDLE session_key_handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr,
                                    0) < 0 ||
        BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(
                              BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0 ||
        BCryptGenerateSymmetricKey(algorithm, &static_key, nullptr, 0,
                                   const_cast<PUCHAR>(image.data() + 0x682D20),
                                   32, 0) < 0) {
        std::fprintf(stderr, "AES-GCM setup failed\n");
        if (static_key)
            BCryptDestroyKey(static_key);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        return 1;
    }

    std::vector<std::uint8_t> envelope(0x30298);
    std::uint8_t session_key[32]{};
    if (BCryptGenRandom(nullptr, session_key, sizeof(session_key),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        BCryptGenRandom(nullptr, envelope.data(), 12,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        BCryptGenRandom(nullptr, envelope.data() + 0x3C, 12,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        BCryptGenerateSymmetricKey(algorithm, &session_key_handle, nullptr, 0,
                                   session_key, sizeof(session_key), 0) < 0) {
        std::fprintf(stderr, "session material generation failed\n");
        if (session_key_handle)
            BCryptDestroyKey(session_key_handle);
        BCryptDestroyKey(static_key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return 1;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO outer{};
    BCRYPT_INIT_AUTH_MODE_INFO(outer);
    outer.pbNonce = envelope.data();
    outer.cbNonce = 12;
    outer.pbTag = envelope.data() + 0x2C;
    outer.cbTag = 16;
    ULONG done = 0;
    NTSTATUS status = BCryptEncrypt(static_key, session_key, sizeof(session_key),
                                    &outer, nullptr, 0,
                                    envelope.data() + 0x0C,
                                    sizeof(session_key), &done, 0);
    if (status < 0 || done != sizeof(session_key)) {
        std::fprintf(stderr, "outer encryption failed: 0x%08lX\n",
                     static_cast<unsigned long>(status));
        SecureZeroMemory(session_key, sizeof(session_key));
        BCryptDestroyKey(session_key_handle);
        BCryptDestroyKey(static_key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return 1;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO inner{};
    BCRYPT_INIT_AUTH_MODE_INFO(inner);
    inner.pbNonce = envelope.data() + 0x3C;
    inner.cbNonce = 12;
    inner.pbTag = envelope.data() + 0x30288;
    inner.cbTag = 16;
    done = 0;
    status = BCryptEncrypt(session_key_handle,
                           const_cast<PUCHAR>(handoff.data()),
                           static_cast<ULONG>(handoff.size()), &inner, nullptr, 0,
                           envelope.data() + 0x48,
                           static_cast<ULONG>(handoff.size()), &done, 0);
    SecureZeroMemory(session_key, sizeof(session_key));
    if (status < 0 || done != handoff.size()) {
        std::fprintf(stderr, "inner encryption failed: 0x%08lX\n",
                     static_cast<unsigned long>(status));
        BCryptDestroyKey(session_key_handle);
        BCryptDestroyKey(static_key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return 1;
    }

    std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(envelope.data()), envelope.size());
    if (!output.good()) {
        std::fprintf(stderr, "output write failed\n");
        BCryptDestroyKey(session_key_handle);
        BCryptDestroyKey(static_key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return 1;
    }
    BCryptDestroyKey(session_key_handle);
    BCryptDestroyKey(static_key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::printf("output=%s bytes=%zu verified-layout=2\n", argv[3],
                envelope.size());
    return 0;
}
