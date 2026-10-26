// INFRASTRUCTURE LAYER: time, password hashing, file storage. Each sits behind an interface
// so tests can replace it and a new algorithm can be added without editing the services.
#pragma once
#include <filesystem>
#include <string>

class IClock {
public:
    virtual ~IClock() = default;
    virtual long long now() const = 0;                     // UTC seconds since 1970
};
class SystemClock : public IClock {
public:
    long long now() const override;
};

class IPasswordHasher {
public:
    virtual ~IPasswordHasher() = default;
    virtual std::string hash(const std::string& password) const = 0;
    virtual bool verify(const std::string& password, const std::string& stored) const = 0;
};
// Argon2id from libsodium. The salt and cost parameters are inside the returned string.
class Argon2Hasher : public IPasswordHasher {
public:
    // Default = libsodium "interactive" cost (about 64 MB, well under the 2 GB limit).
    // Tests pass a tiny cost so they run fast.
    explicit Argon2Hasher(bool fastForTests = false);
    std::string hash(const std::string& password) const override;
    bool verify(const std::string& password, const std::string& stored) const override;
private:
    unsigned long long ops_;
    std::size_t mem_;
};

class IFileStore {
public:
    virtual ~IFileStore() = default;
    // Copies the file into the store and returns the stored path.
    virtual std::string save(const std::string& sourcePath, long long studentId,
                             long long assignmentId, int version) = 0;
    virtual void remove(const std::string& storedPath) = 0;
};
class LocalFileStore : public IFileStore {
public:
    explicit LocalFileStore(std::filesystem::path folder);
    std::string save(const std::string& sourcePath, long long studentId,
                     long long assignmentId, int version) override;
    void remove(const std::string& storedPath) override;
private:
    std::filesystem::path folder_;
};
