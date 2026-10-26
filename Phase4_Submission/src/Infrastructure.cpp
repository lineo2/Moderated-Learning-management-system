#include "Infrastructure.h"

#include <sodium.h>

#include <chrono>

#include "Domain.h"

long long SystemClock::now() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

Argon2Hasher::Argon2Hasher(bool fastForTests) {
    if (sodium_init() < 0) throw AppException("Could not start the cryptography library.");
    ops_ = fastForTests ? crypto_pwhash_OPSLIMIT_MIN : crypto_pwhash_OPSLIMIT_INTERACTIVE;
    mem_ = fastForTests ? crypto_pwhash_MEMLIMIT_MIN : crypto_pwhash_MEMLIMIT_INTERACTIVE;
}
std::string Argon2Hasher::hash(const std::string& password) const {
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out, password.c_str(), password.size(), ops_, mem_) != 0)
        throw AppException("Password hashing failed (out of memory).");
    return out;
}
bool Argon2Hasher::verify(const std::string& password, const std::string& stored) const {
    return crypto_pwhash_str_verify(stored.c_str(), password.c_str(), password.size()) == 0;
}

LocalFileStore::LocalFileStore(std::filesystem::path folder) : folder_(std::move(folder)) {
    std::filesystem::create_directories(folder_);
}
std::string LocalFileStore::save(const std::string& sourcePath, long long studentId,
                                 long long assignmentId, int version) {
    namespace fs = std::filesystem;
    std::string ext = fs::path(sourcePath).extension().string();
    if (ext.size() > 10) ext.clear();
    fs::path target = folder_ / ("s" + std::to_string(studentId) + "_a" + std::to_string(assignmentId) +
                                 "_v" + std::to_string(version) + ext);
    std::error_code ec;
    fs::copy_file(sourcePath, target, fs::copy_options::overwrite_existing, ec);
    if (ec) throw AppException("Could not store the file: " + ec.message());
    return target.string();
}
void LocalFileStore::remove(const std::string& storedPath) {
    std::error_code ec;
    std::filesystem::remove(storedPath, ec);
}
