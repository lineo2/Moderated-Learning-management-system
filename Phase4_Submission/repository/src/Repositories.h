// DATA ACCESS LAYER (part 2): one repository per group of tables. All SQL in the program is here.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "Database.h"
#include "Domain.h"

struct AssignmentView {                   // an assignment plus what a list screen needs
    Assignment assignment;
    std::string courseCode, submissionState;   // submissionState is "" when the student has not submitted
    long long effectiveDeadline = 0;
};
struct AuditEntry {
    long long id = 0, submissionId = 0, changedBy = 0, changedAt = 0;
    std::string action, reason, username;
    bool hasOld = false;
    long long oldRaw = 0, newRaw = 0, oldFinal = 0, newFinal = 0;
    bool actingCapacity = false;
};

class UserRepository {
public:
    explicit UserRepository(Database& db) : db_(db) {}
    std::optional<UserRecord> findById(long long id);
    std::optional<UserRecord> findByUsername(const std::string& username);
    long long insert(const UserRecord& u);
    void updatePassword(long long id, const std::string& hash, bool mustChange);
    void setActive(long long id, bool active);
    std::vector<UserRecord> list();
private:
    Database& db_;
};

class AcademicRepository {                // departments, courses, teaching, enrolment, acting HoD
public:
    explicit AcademicRepository(Database& db) : db_(db) {}
    long long insertDepartment(const std::string& name);
    std::optional<Department> findDepartment(long long id);
    std::vector<Department> listDepartments();
    void setHead(long long departmentId, long long userId);
    long long insertCourse(long long departmentId, const std::string& code, const std::string& title);
    std::optional<Course> findCourse(long long id);
    std::vector<Course> listCourses();
    std::vector<Course> coursesForStudent(long long studentId);
    std::vector<Course> coursesForLecturer(long long lecturerId);
    void addLecturer(long long courseId, long long lecturerId);
    void enrol(long long courseId, long long studentId, long long now);
    bool teaches(long long userId, long long courseId);
    bool isEnrolled(long long courseId, long long studentId);
    std::vector<long long> headedDepartments(long long userId);     // department.head_user_id
    std::vector<long long> actingDepartments(long long userId);     // active acting_hod rows
    void designateActing(long long departmentId, long long lecturerId, long long now);
private:
    Database& db_;
};

class AssignmentRepository {
public:
    explicit AssignmentRepository(Database& db) : db_(db) {}
    long long insert(const Assignment& a);
    std::optional<Assignment> find(long long id);
    void close(long long id);
    std::vector<AssignmentView> listForLecturer(long long lecturerId);
    std::vector<AssignmentView> listForStudent(long long studentId);
    long long effectiveDeadline(long long assignmentId, long long studentId, long long baseDeadline);
    void insertExtension(long long assignmentId, long long studentId, long long grantedBy,
                         long long oldDeadline, long long newDeadline, const std::string& reason, long long now);
private:
    Database& db_;
};

class SubmissionRepository {
public:
    explicit SubmissionRepository(Database& db) : db_(db) {}
    std::optional<SubmissionDetail> find(long long id);
    std::optional<SubmissionDetail> findFor(long long assignmentId, long long studentId);
    std::vector<SubmissionDetail> listForAssignment(long long assignmentId);
    std::vector<SubmissionDetail> listForLecturer(long long lecturerId, State state);
    std::vector<SubmissionDetail> listForDepartment(long long departmentId, State state);
    std::vector<SubmissionDetail> listReleasedForStudent(long long studentId);
    int versionCount(long long submissionId);
    long long insertSubmission(long long assignmentId, long long studentId);
    void insertVersion(long long submissionId, int versionNo, const std::string& originalName,
                       const std::string& storedPath, long long size, long long submittedAt);
    // Each update only succeeds if the state is still `expected`; false means someone changed it first.
    bool saveMark(long long id, State expected, State next, long long raw, const std::string& feedback, long long markedBy);
    bool saveApproval(long long id, State expected, State next, long long moderatorId, const std::string& comment);
    bool saveAdjustment(long long id, State expected, State next, long long raw, long long moderatorId, const std::string& comment);
    bool saveReturn(long long id, State expected, State next, const std::string& comment);
    bool saveRelease(long long id, State expected, State next, long long now);
    bool saveAmendment(long long id, State expected, long long raw);
private:
    std::vector<SubmissionDetail> select(const std::string& where, const std::vector<Value>& params);
    Database& db_;
};

class AuditRepository {
public:
    explicit AuditRepository(Database& db) : db_(db) {}
    void insert(const AuditEntry& e);
    std::vector<AuditEntry> latest(int limit);
private:
    Database& db_;
};
