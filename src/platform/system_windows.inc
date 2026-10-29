std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), n);
    return out;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode text");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
    return out;
}
static std::string hashFile(const Path& path, bool sha256, std::atomic_bool& stop) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, sha256 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error(sha256 ? "Cannot initialize SHA-256" : "Cannot initialize SHA-1");
    struct Cleanup { BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h; ~Cleanup() { if(h) BCryptDestroyHash(h); if(a) BCryptCloseAlgorithmProvider(a,0); } } cleanup{alg,hash};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("Cannot create hash");
    std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("Cannot read file for verification");
    char buffer[65536];
    while (in) {
        if (stop) throw std::runtime_error("Paused");
        in.read(buffer, sizeof buffer);
        if (BCryptHashData(hash, (PUCHAR)buffer, (ULONG)in.gcount(), 0) < 0) throw std::runtime_error("Hash failed");
    }
    if (!in.eof()) throw std::runtime_error("File read failed during verification");
    unsigned char digest[32]; const ULONG length = sha256 ? 32 : 20;
    if (BCryptFinishHash(hash, digest, length, 0) < 0) throw std::runtime_error("Hash failed");
    std::ostringstream out; for (ULONG i = 0; i < length; ++i) out << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return out.str();
}
std::string sha1File(const Path& path, std::atomic_bool& stop) { return hashFile(path, false, stop); }
std::string sha256File(const Path& path, std::atomic_bool& stop) { return hashFile(path, true, stop); }