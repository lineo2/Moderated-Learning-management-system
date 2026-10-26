#include "Repositories.h"

namespace {
Value I(long long v) { return v; }
Value S(const std::string& v) { return v; }

UserRecord toUser(const Row& r) {
    UserRecord u;
    u.id = r.num(0); u.username = r.str(1); u.fullName = r.str(2); u.role = roleFromText(r.str(3));
    u.passwordHash = r.str(4); u.mustChangePassword = r.num(5) != 0; u.active = r.num(6) != 0;
    return u;
}
const char* kUserCols = "user_id,username,full_name,role,password_hash,must_change_password,is_active";

Course toCourse(const Row& r) { return {r.num(0), r.num(1), r.str(2), r.str(3)}; }
const char* kCourseCols = "c.course_id,c.department_id,c.code,c.title";

Assignment toAssignment(const Row& r) {
    Assignment a;
    a.id = r.num(0); a.courseId = r.num(1); a.title = r.str(2); a.description = r.str(3);
    a.deadline = r.num(4); a.maxMark = r.num(5); a.closed = r.num(6) != 0; a.createdBy = r.num(7);
    return a;
}
const char* kAssignCols = "a.assignment_id,a.course_id,a.title,a.description,a.deadline,a.max_mark,a.is_closed,a.created_by";
// Effective deadline = later of the assignment deadline and the student's latest extension.
const char* kEffective =
    "MAX(a.deadline, COALESCE((SELECT MAX(e.new_deadline) FROM extension e "
    "WHERE e.assignment_id=a.assignment_id AND e.student_id=?),0))";

std::vector<long long> ids(const std::vector<Row>& rows) {
    std::vector<long long> out;
    for (const auto& r : rows) out.push_back(r.num(0));
    return out;
}
}  // namespace

// ---------- UserRepository ----------
std::optional<UserRecord> UserRepository::findById(long long id) {
    auto rows = db_.query(std::string("SELECT ") + kUserCols + " FROM app_user WHERE user_id=?", {I(id)});
    if (rows.empty()) return std::nullopt;
    return toUser(rows[0]);
}
std::optional<UserRecord> UserRepository::findByUsername(const std::string& username) {
    auto rows = db_.query(std::string("SELECT ") + kUserCols + " FROM app_user WHERE username=?", {S(username)});
    if (rows.empty()) return std::nullopt;
    return toUser(rows[0]);
}
long long UserRepository::insert(const UserRecord& u) {
    db_.execute("INSERT INTO app_user(username,full_name,role,password_hash,must_change_password,is_active) VALUES(?,?,?,?,?,1)",
                {S(u.username), S(u.fullName), S(roleToText(u.role)), S(u.passwordHash), I(u.mustChangePassword)});
    return db_.lastInsertId();
}
void UserRepository::updatePassword(long long id, const std::string& hash, bool mustChange) {
    db_.execute("UPDATE app_user SET password_hash=?, must_change_password=? WHERE user_id=?", {S(hash), I(mustChange), I(id)});
}
void UserRepository::setActive(long long id, bool active) {
    db_.execute("UPDATE app_user SET is_active=? WHERE user_id=?", {I(active), I(id)});
}
std::vector<UserRecord> UserRepository::list() {
    std::vector<UserRecord> out;
    for (const auto& r : db_.query(std::string("SELECT ") + kUserCols + " FROM app_user ORDER BY user_id")) out.push_back(toUser(r));
    return out;
}

// ---------- AcademicRepository ----------
long long AcademicRepository::insertDepartment(const std::string& name) {
    db_.execute("INSERT INTO department(name) VALUES(?)", {S(name)});
    return db_.lastInsertId();
}
std::optional<Department> AcademicRepository::findDepartment(long long id) {
    auto rows = db_.query("SELECT department_id,name,COALESCE(head_user_id,0) FROM department WHERE department_id=?", {I(id)});
    if (rows.empty()) return std::nullopt;
    return Department{rows[0].num(0), rows[0].str(1), rows[0].num(2)};
}
std::vector<Department> AcademicRepository::listDepartments() {
    std::vector<Department> out;
    for (const auto& r : db_.query("SELECT department_id,name,COALESCE(head_user_id,0) FROM department ORDER BY department_id"))
        out.push_back({r.num(0), r.str(1), r.num(2)});
    return out;
}
void AcademicRepository::setHead(long long departmentId, long long userId) {
    db_.execute("UPDATE department SET head_user_id=? WHERE department_id=?", {I(userId), I(departmentId)});
}
long long AcademicRepository::insertCourse(long long departmentId, const std::string& code, const std::string& title) {
    db_.execute("INSERT INTO course(department_id,code,title) VALUES(?,?,?)", {I(departmentId), S(code), S(title)});
    return db_.lastInsertId();
}
std::optional<Course> AcademicRepository::findCourse(long long id) {
    auto rows = db_.query(std::string("SELECT ") + kCourseCols + " FROM course c WHERE c.course_id=?", {I(id)});
    if (rows.empty()) return std::nullopt;
    return toCourse(rows[0]);
}
std::vector<Course> AcademicRepository::listCourses() {
    std::vector<Course> out;
    for (const auto& r : db_.query(std::string("SELECT ") + kCourseCols + " FROM course c ORDER BY c.course_id")) out.push_back(toCourse(r));
    return out;
}
std::vector<Course> AcademicRepository::coursesForStudent(long long studentId) {
    std::vector<Course> out;
    for (const auto& r : db_.query(std::string("SELECT ") + kCourseCols +
             " FROM course c JOIN enrolment e ON e.course_id=c.course_id WHERE e.student_id=? ORDER BY c.code", {I(studentId)}))
        out.push_back(toCourse(r));
    return out;
}
std::vector<Course> AcademicRepository::coursesForLecturer(long long lecturerId) {
    std::vector<Course> out;
    for (const auto& r : db_.query(std::string("SELECT ") + kCourseCols +
             " FROM course c JOIN course_lecturer l ON l.course_id=c.course_id WHERE l.lecturer_id=? ORDER BY c.code", {I(lecturerId)}))
        out.push_back(toCourse(r));
    return out;
}
void AcademicRepository::addLecturer(long long courseId, long long lecturerId) {
    db_.execute("INSERT INTO course_lecturer(course_id,lecturer_id) VALUES(?,?)", {I(courseId), I(lecturerId)});
}
void AcademicRepository::enrol(long long courseId, long long studentId, long long now) {
    db_.execute("INSERT INTO enrolment(course_id,student_id,enrolled_on) VALUES(?,?,?)", {I(courseId), I(studentId), I(now)});
}
bool AcademicRepository::teaches(long long userId, long long courseId) {
    return !db_.query("SELECT 1 FROM course_lecturer WHERE course_id=? AND lecturer_id=?", {I(courseId), I(userId)}).empty();
}
bool AcademicRepository::isEnrolled(long long courseId, long long studentId) {
    return !db_.query("SELECT 1 FROM enrolment WHERE course_id=? AND student_id=?", {I(courseId), I(studentId)}).empty();
}
std::vector<long long> AcademicRepository::headedDepartments(long long userId) {
    return ids(db_.query("SELECT department_id FROM department WHERE head_user_id=?", {I(userId)}));
}
std::vector<long long> AcademicRepository::actingDepartments(long long userId) {
    return ids(db_.query("SELECT department_id FROM acting_hod WHERE lecturer_id=? AND end_at IS NULL", {I(userId)}));
}
void AcademicRepository::designateActing(long long departmentId, long long lecturerId, long long now) {
    // End the current designation (if any), then start the new one. end_at must be after start_at.
    db_.execute("UPDATE acting_hod SET end_at = MAX(?, start_at + 1) WHERE department_id=? AND end_at IS NULL",
                {I(now), I(departmentId)});
    db_.execute("INSERT INTO acting_hod(department_id,lecturer_id,start_at) VALUES(?,?,?)",
                {I(departmentId), I(lecturerId), I(now)});
}

// ---------- AssignmentRepository ----------
long long AssignmentRepository::insert(const Assignment& a) {
    db_.execute("INSERT INTO assignment(course_id,title,description,deadline,max_mark,created_by) VALUES(?,?,?,?,?,?)",
                {I(a.courseId), S(a.title), S(a.description), I(a.deadline), I(a.maxMark), I(a.createdBy)});
    return db_.lastInsertId();
}
std::optional<Assignment> AssignmentRepository::find(long long id) {
    auto rows = db_.query(std::string("SELECT ") + kAssignCols + " FROM assignment a WHERE a.assignment_id=?", {I(id)});
    if (rows.empty()) return std::nullopt;
    return toAssignment(rows[0]);
}
void AssignmentRepository::close(long long id) {
    db_.execute("UPDATE assignment SET is_closed=1 WHERE assignment_id=?", {I(id)});
}
std::vector<AssignmentView> AssignmentRepository::listForLecturer(long long lecturerId) {
    std::vector<AssignmentView> out;
    auto rows = db_.query(std::string("SELECT ") + kAssignCols + ",c.code FROM assignment a JOIN course c ON c.course_id=a.course_id "
        "WHERE a.course_id IN (SELECT course_id FROM course_lecturer WHERE lecturer_id=?) ORDER BY a.deadline", {I(lecturerId)});
    for (const auto& r : rows) out.push_back({toAssignment(r), r.str(8), "", r.num(4)});
    return out;
}
std::vector<AssignmentView> AssignmentRepository::listForStudent(long long studentId) {
    std::vector<AssignmentView> out;
    auto rows = db_.query(std::string("SELECT ") + kAssignCols + ",c.code," + kEffective + ",COALESCE(s.state,'') "
        "FROM assignment a JOIN course c ON c.course_id=a.course_id JOIN enrolment en ON en.course_id=c.course_id "
        "LEFT JOIN submission s ON s.assignment_id=a.assignment_id AND s.student_id=? "
        "WHERE en.student_id=? ORDER BY a.deadline", {I(studentId), I(studentId), I(studentId)});
    for (const auto& r : rows) out.push_back({toAssignment(r), r.str(8), r.str(10), r.num(9)});
    return out;
}
long long AssignmentRepository::effectiveDeadline(long long assignmentId, long long studentId, long long) {
    auto rows = db_.query(std::string("SELECT ") + kEffective + " FROM assignment a WHERE a.assignment_id=?",
                          {I(studentId), I(assignmentId)});
    return rows.empty() ? 0 : rows[0].num(0);
}
void AssignmentRepository::insertExtension(long long assignmentId, long long studentId, long long grantedBy,
        long long oldDeadline, long long newDeadline, const std::string& reason, long long now) {
    db_.execute("INSERT INTO extension(assignment_id,student_id,granted_by,old_deadline,new_deadline,reason,granted_at) VALUES(?,?,?,?,?,?,?)",
                {I(assignmentId), I(studentId), I(grantedBy), I(oldDeadline), I(newDeadline), S(reason), I(now)});
}

// ---------- SubmissionRepository ----------
std::vector<SubmissionDetail> SubmissionRepository::select(const std::string& where, const std::vector<Value>& params) {
    std::string sql =
        "SELECT s.submission_id,s.assignment_id,a.course_id,c.department_id,s.student_id,a.title,c.code,u.full_name,"
        " s.feedback,s.moderator_comment,"
        " COALESCE((SELECT v.stored_path FROM submission_version v WHERE v.submission_id=s.submission_id ORDER BY v.version_no DESC LIMIT 1),''),"
        " s.state,s.raw_tenths,a.max_mark,"
        " MAX(a.deadline, COALESCE((SELECT MAX(e.new_deadline) FROM extension e WHERE e.assignment_id=s.assignment_id AND e.student_id=s.student_id),0)),"
        " COALESCE((SELECT v.submitted_at FROM submission_version v WHERE v.submission_id=s.submission_id ORDER BY v.version_no DESC LIMIT 1),0),"
        " s.marked_by,s.moderated_by,s.released_at,"
        " COALESCE((SELECT MAX(v.version_no) FROM submission_version v WHERE v.submission_id=s.submission_id),0)"
        " FROM submission s JOIN assignment a ON a.assignment_id=s.assignment_id"
        " JOIN course c ON c.course_id=a.course_id JOIN app_user u ON u.user_id=s.student_id " + where;
    std::vector<SubmissionDetail> out;
    for (const auto& r : db_.query(sql, params)) {
        SubmissionDetail d;
        d.id = r.num(0); d.assignmentId = r.num(1); d.courseId = r.num(2); d.departmentId = r.num(3);
        d.studentId = r.num(4); d.assignmentTitle = r.str(5); d.courseCode = r.str(6); d.studentName = r.str(7);
        d.feedback = r.str(8); d.moderatorComment = r.str(9); d.latestPath = r.str(10);
        d.state = stateFromText(r.str(11)); d.hasMark = !r.isNull(12); d.rawTenths = r.num(12);
        d.maxMark = r.num(13); d.effectiveDeadline = r.num(14); d.latestSubmittedAt = r.num(15);
        d.markedBy = r.num(16); d.moderatedBy = r.num(17); d.releasedAt = r.num(18);
        d.latestVersion = static_cast<int>(r.num(19));
        out.push_back(d);
    }
    return out;
}
std::optional<SubmissionDetail> SubmissionRepository::find(long long id) {
    auto v = select("WHERE s.submission_id=?", {I(id)});
    if (v.empty()) return std::nullopt;
    return v[0];
}
std::optional<SubmissionDetail> SubmissionRepository::findFor(long long assignmentId, long long studentId) {
    auto v = select("WHERE s.assignment_id=? AND s.student_id=?", {I(assignmentId), I(studentId)});
    if (v.empty()) return std::nullopt;
    return v[0];
}
std::vector<SubmissionDetail> SubmissionRepository::listForAssignment(long long assignmentId) {
    return select("WHERE s.assignment_id=? ORDER BY s.submission_id", {I(assignmentId)});
}
std::vector<SubmissionDetail> SubmissionRepository::listForLecturer(long long lecturerId, State state) {
    return select("WHERE s.state=? AND a.course_id IN (SELECT course_id FROM course_lecturer WHERE lecturer_id=?) "
                  "ORDER BY s.submission_id", {S(stateToText(state)), I(lecturerId)});
}
std::vector<SubmissionDetail> SubmissionRepository::listForDepartment(long long departmentId, State state) {
    return select("WHERE s.state=? AND c.department_id=? ORDER BY s.submission_id", {S(stateToText(state)), I(departmentId)});
}
std::vector<SubmissionDetail> SubmissionRepository::listReleasedForStudent(long long studentId) {
    return select("WHERE s.student_id=? AND s.state='RELEASED' ORDER BY a.deadline", {I(studentId)});
}
int SubmissionRepository::versionCount(long long submissionId) {
    return static_cast<int>(db_.query("SELECT COUNT(*) FROM submission_version WHERE submission_id=?", {I(submissionId)})[0].num(0));
}
long long SubmissionRepository::insertSubmission(long long assignmentId, long long studentId) {
    db_.execute("INSERT INTO submission(assignment_id,student_id) VALUES(?,?)", {I(assignmentId), I(studentId)});
    return db_.lastInsertId();
}
void SubmissionRepository::insertVersion(long long submissionId, int versionNo, const std::string& originalName,
        const std::string& storedPath, long long size, long long submittedAt) {
    db_.execute("INSERT INTO submission_version(submission_id,version_no,original_name,stored_path,file_size,submitted_at) VALUES(?,?,?,?,?,?)",
                {I(submissionId), I(versionNo), S(originalName), S(storedPath), I(size), I(submittedAt)});
}
bool SubmissionRepository::saveMark(long long id, State expected, State next, long long raw,
                                    const std::string& feedback, long long markedBy) {
    return db_.execute("UPDATE submission SET state=?,raw_tenths=?,feedback=?,marked_by=? WHERE submission_id=? AND state=?",
                       {S(stateToText(next)), I(raw), S(feedback), I(markedBy), I(id), S(stateToText(expected))}) == 1;
}
bool SubmissionRepository::saveApproval(long long id, State expected, State next, long long moderatorId, const std::string& comment) {
    return db_.execute("UPDATE submission SET state=?,moderated_by=?,moderator_comment=? WHERE submission_id=? AND state=?",
                       {S(stateToText(next)), I(moderatorId), S(comment), I(id), S(stateToText(expected))}) == 1;
}
bool SubmissionRepository::saveAdjustment(long long id, State expected, State next, long long raw,
                                          long long moderatorId, const std::string& comment) {
    return db_.execute("UPDATE submission SET state=?,raw_tenths=?,moderated_by=?,moderator_comment=? WHERE submission_id=? AND state=?",
                       {S(stateToText(next)), I(raw), I(moderatorId), S(comment), I(id), S(stateToText(expected))}) == 1;
}
bool SubmissionRepository::saveReturn(long long id, State expected, State next, const std::string& comment) {
    return db_.execute("UPDATE submission SET state=?,moderator_comment=? WHERE submission_id=? AND state=?",
                       {S(stateToText(next)), S(comment), I(id), S(stateToText(expected))}) == 1;
}
bool SubmissionRepository::saveRelease(long long id, State expected, State next, long long now) {
    return db_.execute("UPDATE submission SET state=?,released_at=? WHERE submission_id=? AND state=?",
                       {S(stateToText(next)), I(now), I(id), S(stateToText(expected))}) == 1;
}
bool SubmissionRepository::saveAmendment(long long id, State expected, long long raw) {
    return db_.execute("UPDATE submission SET raw_tenths=? WHERE submission_id=? AND state=?",
                       {I(raw), I(id), S(stateToText(expected))}) == 1;
}

// ---------- AuditRepository ----------
void AuditRepository::insert(const AuditEntry& e) {
    db_.execute("INSERT INTO audit_entry(submission_id,changed_by,changed_at,action,old_raw_tenths,new_raw_tenths,"
                "old_final_tenths,new_final_tenths,reason,acting_capacity) VALUES(?,?,?,?,?,?,?,?,?,?)",
                {I(e.submissionId), I(e.changedBy), I(e.changedAt), S(e.action),
                 e.hasOld ? Value(e.oldRaw) : null(), I(e.newRaw), e.hasOld ? Value(e.oldFinal) : null(),
                 I(e.newFinal), S(e.reason), I(e.actingCapacity)});
}
std::vector<AuditEntry> AuditRepository::latest(int limit) {
    std::vector<AuditEntry> out;
    auto rows = db_.query("SELECT a.audit_id,a.submission_id,a.changed_by,a.changed_at,a.action,a.old_raw_tenths,a.new_raw_tenths,"
                          "a.old_final_tenths,a.new_final_tenths,a.reason,a.acting_capacity,u.username "
                          "FROM audit_entry a JOIN app_user u ON u.user_id=a.changed_by ORDER BY a.audit_id DESC LIMIT ?", {I(limit)});
    for (const auto& r : rows) {
        AuditEntry e;
        e.id = r.num(0); e.submissionId = r.num(1); e.changedBy = r.num(2); e.changedAt = r.num(3); e.action = r.str(4);
        e.hasOld = !r.isNull(5); e.oldRaw = r.num(5); e.newRaw = r.num(6); e.oldFinal = r.num(7); e.newFinal = r.num(8);
        e.reason = r.str(9); e.actingCapacity = r.num(10) != 0; e.username = r.str(11);
        out.push_back(e);
    }
    return out;
}
