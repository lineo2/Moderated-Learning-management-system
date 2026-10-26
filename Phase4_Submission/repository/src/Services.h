// APPLICATION LAYER: one method per use case. Each method checks permission and rules,
// controls the transaction and calls the repositories.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Database.h"
#include "Domain.h"
#include "Infrastructure.h"
#include "Repositories.h"

struct Session {                           // who is logged in and when they last did something
    std::unique_ptr<User> user;
    long long lastActive = 0;
};

class AuditService {
public:
    explicit AuditService(AuditRepository& repo) : repo_(repo) {}
    void record(const AuditEntry& e) { repo_.insert(e); }          // there is no update or delete
    std::vector<AuditEntry> view(const User& actor, int limit = 100);
private:
    AuditRepository& repo_;
};

class AuthService {
public:
    static constexpr long long kIdleLimitSeconds = 10 * 60;
    AuthService(UserRepository& users, IPasswordHasher& hasher, IClock& clock)
        : users_(users), hasher_(hasher), clock_(clock) {}
    Session login(const std::string& username, const std::string& password);
    void changePassword(Session& s, const std::string& newPassword);
    void logout(Session& s) { s.user.reset(); }
    void checkIdle(Session& s);            // throws SessionExpiredException after 10 idle minutes
    void touch(Session& s) { s.lastActive = clock_.now(); }
private:
    UserRepository& users_;
    IPasswordHasher& hasher_;
    IClock& clock_;
};

class AdminService {
public:
    AdminService(Database& db, UserRepository& users, AcademicRepository& academic,
                 IPasswordHasher& hasher, IClock& clock)
        : db_(db), users_(users), academic_(academic), hasher_(hasher), clock_(clock) {}
    bool createFirstAdminIfNeeded(const std::string& password);    // true if it was created
    long long createUser(const User& actor, const std::string& username, const std::string& fullName,
                         Role role, const std::string& tempPassword);
    void deactivate(const User& actor, long long userId);
    void resetPassword(const User& actor, long long userId, const std::string& tempPassword);
    long long createDepartment(const User& actor, const std::string& name);
    void setHead(const User& actor, long long departmentId, long long hodUserId);
    long long createCourse(const User& actor, long long departmentId, const std::string& code, const std::string& title);
    void assignLecturer(const User& actor, long long courseId, long long lecturerId);
    void enrol(const User& actor, long long courseId, long long studentId);
    // CSV lines: username,full name,temporary password[,course code]. All lines succeed or none do.
    int importCsv(const User& actor, const std::string& path);
    void designateActingHod(const User& actor, long long departmentId, long long lecturerId);
    std::vector<UserRecord> listUsers(const User& actor);
    std::vector<Department> listDepartments(const User& actor);
    std::vector<Course> listCourses(const User& actor);
private:
    long long insertUser(const std::string& username, const std::string& fullName, Role role, const std::string& tempPassword);
    void enrolChecked(long long courseId, long long studentId);
    Database& db_;
    UserRepository& users_;
    AcademicRepository& academic_;
    IPasswordHasher& hasher_;
    IClock& clock_;
};

class AssignmentService {
public:
    AssignmentService(AssignmentRepository& assignments, AcademicRepository& academic, IClock& clock)
        : assignments_(assignments), academic_(academic), clock_(clock) {}
    long long create(const User& actor, long long courseId, const std::string& title,
                     const std::string& description, long long deadline, long long maxMark);
    void close(const User& actor, long long assignmentId);
    void grantExtension(const User& actor, long long assignmentId, long long studentId,
                        long long newDeadline, const std::string& reason);
    std::vector<AssignmentView> listForLecturer(const User& actor);
    std::vector<AssignmentView> listForStudent(const User& actor);
    std::vector<Course> coursesForStudent(const User& actor);
    std::vector<Course> coursesForLecturer(const User& actor);
private:
    Assignment requireTaught(const User& actor, long long assignmentId);
    AssignmentRepository& assignments_;
    AcademicRepository& academic_;
    IClock& clock_;
};

struct SubmissionPlan {                    // what prepare() found out, shown to the student before confirm()
    long long assignmentId = 0, effectiveDeadline = 0, now = 0;
    int version = 1, daysLate = 0;
    bool isResubmission = false;
    std::string sourcePath;
};
struct Receipt { long long submissionId = 0, submittedAt = 0; int version = 0, daysLate = 0; };
struct MarkView {                          // what a student sees after release
    std::string courseCode, title, feedback;
    long long rawTenths = 0, penaltyTenths = 0, finalTenths = 0, maxMark = 0;
    int daysLate = 0;
};

class SubmissionService {
public:
    SubmissionService(Database& db, SubmissionRepository& subs, AssignmentRepository& assignments,
                      AcademicRepository& academic, IFileStore& files, IClock& clock, const PenaltyPolicy& policy)
        : db_(db), subs_(subs), assignments_(assignments), academic_(academic), files_(files), clock_(clock), policy_(policy) {}
    SubmissionPlan prepare(const User& student, long long assignmentId, const std::string& sourcePath);
    Receipt confirm(const User& student, const SubmissionPlan& plan);
    std::vector<MarkView> viewMarks(const User& student);          // only RELEASED submissions
private:
    SubmissionPlan check(const User& student, long long assignmentId, const std::string& sourcePath);
    Database& db_;
    SubmissionRepository& subs_;
    AssignmentRepository& assignments_;
    AcademicRepository& academic_;
    IFileStore& files_;
    IClock& clock_;
    const PenaltyPolicy& policy_;
};

class MarkingService {
public:
    MarkingService(Database& db, SubmissionRepository& subs, AssignmentRepository& assignments, AcademicRepository& academic,
                   AuditService& audit, const PenaltyPolicy& policy, IClock& clock)
        : db_(db), subs_(subs), assignments_(assignments), academic_(academic), audit_(audit), policy_(policy), clock_(clock) {}
    std::vector<SubmissionDetail> listSubmissions(const User& actor, long long assignmentId);
    std::vector<SubmissionDetail> listUnmarked(const User& actor);   // lecturer dashboard
    long long previewFinal(const SubmissionDetail& d, long long rawTenths) const {   // penalty breakdown before saving
        return policy_.finalTenths(rawTenths, d.maxMark, d.daysLate);
    }
    // reason is required when changing an existing mark (revise, or re-mark after return).
    void mark(const User& actor, long long submissionId, long long rawTenths,
              const std::string& feedback, const std::string& reason);
private:
    Database& db_;
    SubmissionRepository& subs_;
    AssignmentRepository& assignments_;
    AcademicRepository& academic_;
    AuditService& audit_;
    const PenaltyPolicy& policy_;
    IClock& clock_;
};

class ModerationService {
public:
    ModerationService(Database& db, SubmissionRepository& subs, AcademicRepository& academic,
                      AuditService& audit, const PenaltyPolicy& policy, IClock& clock)
        : db_(db), subs_(subs), academic_(academic), audit_(audit), policy_(policy), clock_(clock) {}
    std::vector<long long> departmentsFor(const User& actor);       // headed or acting; empty if none
    std::vector<SubmissionDetail> queue(const User& actor);         // MARKED, not taught by actor
    std::vector<SubmissionDetail> approvedQueue(const User& actor); // MODERATED, ready to release
    void approve(const User& actor, long long submissionId, const std::string& comment);
    void adjust(const User& actor, long long submissionId, long long newRawTenths, const std::string& reason);
    void returnToLecturer(const User& actor, long long submissionId, const std::string& comment);
    int release(const User& actor, const std::vector<long long>& submissionIds);   // all or none
    void amend(const User& actor, long long submissionId, long long newRawTenths, const std::string& reason);
private:
    struct Checked { SubmissionDetail detail; bool acting; };
    Checked load(const User& actor, long long submissionId);
    void record(const User& actor, const Checked& c, const char* action, bool hasOld, long long oldRaw,
                long long newRaw, const std::string& reason);
    Database& db_;
    SubmissionRepository& subs_;
    AcademicRepository& academic_;
    AuditService& audit_;
    const PenaltyPolicy& policy_;
    IClock& clock_;
};

class BackupService {
public:
    explicit BackupService(Database& db) : db_(db) {}
    void backup(const User& actor, const std::string& path);
private:
    Database& db_;
};
