// DOMAIN LAYER: business objects and rules. Needs no database and no screen.
#pragma once
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// ---------- Errors (one hierarchy; the Console layer catches AppException) ----------
struct AppException : std::runtime_error { using std::runtime_error::runtime_error; };
struct ValidationException : AppException { using AppException::AppException; };
struct AuthorizationException : AppException { using AppException::AppException; };
struct SessionExpiredException : AuthorizationException { using AuthorizationException::AuthorizationException; };
struct NotFoundException : AppException { using AppException::AppException; };
struct BusinessRuleException : AppException { using AppException::AppException; };
struct IllegalTransitionException : BusinessRuleException { using BusinessRuleException::BusinessRuleException; };
struct DatabaseException : AppException { using AppException::AppException; };

// ---------- Roles, permissions and the User hierarchy (polymorphism + Factory Method) ----------
enum class Role { Admin, Lecturer, Hod, Student };
std::string roleToText(Role r);          // "ADMIN", "LECTURER", "HOD", "STUDENT"
Role roleFromText(const std::string& s);

enum class Permission {
    ManageUsers, ManageCourses, ViewAudit, Backup,                  // administration
    CreateAssignment, GrantExtension, MarkSubmission,               // teaching
    Moderate,                                                       // head of department
    SubmitWork, ViewOwnMarks                                        // student
};

struct UserRecord {                       // one row of app_user
    long long id = 0;
    std::string username, fullName;
    Role role = Role::Student;
    std::string passwordHash;
    bool mustChangePassword = true;
    bool active = true;
};

class User {                              // abstract base
public:
    explicit User(UserRecord r) : rec_(std::move(r)) {}
    virtual ~User() = default;
    virtual Role getRole() const = 0;
    virtual std::set<Permission> permissions() const = 0;
    virtual std::string menuTitle() const = 0;
    bool can(Permission p) const { return permissions().count(p) > 0; }
    long long id() const { return rec_.id; }
    const std::string& username() const { return rec_.username; }
    const std::string& fullName() const { return rec_.fullName; }
    bool mustChangePassword() const { return rec_.mustChangePassword; }
private:
    UserRecord rec_;
};
class Administrator : public User {
public:
    using User::User;
    Role getRole() const override { return Role::Admin; }
    std::set<Permission> permissions() const override;
    std::string menuTitle() const override { return "ADMINISTRATOR MENU"; }
};
class Lecturer : public User {
public:
    using User::User;
    Role getRole() const override { return Role::Lecturer; }
    std::set<Permission> permissions() const override;
    std::string menuTitle() const override { return "LECTURER MENU"; }
};
class HeadOfDepartment : public User {
public:
    using User::User;
    Role getRole() const override { return Role::Hod; }
    std::set<Permission> permissions() const override;
    std::string menuTitle() const override { return "HEAD OF DEPARTMENT MENU"; }
};
class Student : public User {
public:
    using User::User;
    Role getRole() const override { return Role::Student; }
    std::set<Permission> permissions() const override;
    std::string menuTitle() const override { return "STUDENT MENU"; }
};
struct UserFactory {                      // Factory Method: row -> right subclass, no switch in services
    static std::unique_ptr<User> create(const UserRecord& r);
};
void requirePermission(const User& u, Permission p);   // role-based access control, one place

// ---------- Plain records ----------
struct Department { long long id = 0; std::string name; long long headUserId = 0; };
struct Course { long long id = 0, departmentId = 0; std::string code, title; };
struct Assignment {
    long long id = 0, courseId = 0, deadline = 0, maxMark = 0, createdBy = 0;
    std::string title, description;
    bool closed = false;
};

// ---------- Submission life cycle (State pattern) ----------
enum class State { Submitted, Marked, Returned, Moderated, Released };
std::string stateToText(State s);        // "SUBMITTED" ... "RELEASED"
State stateFromText(const std::string& s);

// Each concrete state overrides only its legal transitions; every other call throws.
class SubmissionState {
public:
    virtual ~SubmissionState() = default;
    virtual State id() const = 0;
    virtual State resubmit() const;          // student replaces the file
    virtual State mark() const;              // lecturer marks (Submitted) or re-marks (Returned)
    virtual State revise() const;            // lecturer changes a Marked mark
    virtual State approve() const;           // HoD approves
    virtual State adjust() const;            // HoD changes the mark
    virtual State returnToLecturer() const;  // HoD sends back
    virtual State release() const;           // HoD releases to student
    virtual State amend() const;             // HoD corrects a released mark
    static const SubmissionState& of(State s);
protected:
    [[noreturn]] void illegal(const char* what) const;
};

// ---------- Late penalty (Strategy pattern) ----------
class PenaltyPolicy {
public:
    virtual ~PenaltyPolicy() = default;
    // Marks are whole tenths (40.5 -> 405). Result is between 0 and rawTenths.
    virtual long long finalTenths(long long rawTenths, long long maxMark, int daysLate) const = 0;
};
class TenPercentPerDayPolicy : public PenaltyPolicy {   // the rule agreed in Phase 2
public:
    long long finalTenths(long long rawTenths, long long maxMark, int daysLate) const override;
};
class NoPenaltyPolicy : public PenaltyPolicy {
public:
    long long finalTenths(long long rawTenths, long long, int) const override { return rawTenths; }
};
struct DaysLateCalculator {
    // 0 if on time, otherwise lateness / 24 hours rounded up (weekends count).
    static int daysLate(long long submittedAt, long long effectiveDeadline);
};

// ---------- Joined view of a submission (what lists and services pass around) ----------
struct SubmissionDetail {
    long long id = 0, assignmentId = 0, courseId = 0, departmentId = 0, studentId = 0;
    std::string assignmentTitle, courseCode, studentName, feedback, moderatorComment, latestPath;
    State state = State::Submitted;
    bool hasMark = false;
    long long rawTenths = 0, maxMark = 0, effectiveDeadline = 0, latestSubmittedAt = 0;
    long long markedBy = 0, moderatedBy = 0, releasedAt = 0;
    int latestVersion = 0;
    // derived (never stored): filled by fillDerived()
    int daysLate = 0;
    long long finalTenths = 0;
};
void fillDerived(SubmissionDetail& d, const PenaltyPolicy& policy);

// ---------- Validation and formatting helpers ----------
std::string trim(const std::string& s);
long long parseMarkTenths(const std::string& text, long long maxMark);   // "40.5" -> 405, at most 1 decimal
std::string markText(long long tenths);                                  // 405 -> "40.5"
void requireText(const std::string& value, const std::string& field, std::size_t maxLen);
void checkPasswordRule(const std::string& password, const std::string& username);  // >= 8 chars, != username
long long parseDateTime(const std::string& text);                        // "YYYY-MM-DD HH:MM" local -> UTC seconds
std::string formatDateTime(long long seconds);
