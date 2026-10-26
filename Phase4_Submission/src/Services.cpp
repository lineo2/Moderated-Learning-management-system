#include "Services.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {
bool contains(const std::vector<long long>& v, long long x) { return std::find(v.begin(), v.end(), x) != v.end(); }

UserRecord requireUser(UserRepository& users, long long id) {
    auto u = users.findById(id);
    if (!u) throw NotFoundException("User " + std::to_string(id) + " does not exist.");
    return *u;
}
}  // namespace

// ================= AuditService =================
std::vector<AuditEntry> AuditService::view(const User& actor, int limit) {
    requirePermission(actor, Permission::ViewAudit);
    return repo_.latest(limit);
}

// ================= AuthService =================
Session AuthService::login(const std::string& username, const std::string& password) {
    auto rec = users_.findByUsername(username);
    if (!rec || !rec->active || !hasher_.verify(password, rec->passwordHash))
        throw AuthorizationException("Login failed. Check the username and password.");
    return Session{UserFactory::create(*rec), clock_.now()};
}
void AuthService::changePassword(Session& s, const std::string& newPassword) {
    checkPasswordRule(newPassword, s.user->username());
    users_.updatePassword(s.user->id(), hasher_.hash(newPassword), false);
}
void AuthService::checkIdle(Session& s) {
    if (clock_.now() - s.lastActive > kIdleLimitSeconds) {
        logout(s);
        throw SessionExpiredException("You were idle for more than 10 minutes. Please log in again.");
    }
}

// ================= AdminService =================
bool AdminService::createFirstAdminIfNeeded(const std::string& password) {
    for (const auto& u : users_.list())
        if (u.role == Role::Admin) return false;
    insertUser("admin", "System Administrator", Role::Admin, password);   // must change password at first login
    return true;
}
long long AdminService::insertUser(const std::string& username, const std::string& fullName,
                                   Role role, const std::string& tempPassword) {
    requireText(username, "Username", 80);
    if (username.find_first_of(" ,\t") != std::string::npos)
        throw ValidationException("The username must not contain spaces or commas.");
    requireText(fullName, "Full name", 120);
    checkPasswordRule(tempPassword, username);
    if (users_.findByUsername(username)) throw ValidationException("That username already exists.");
    UserRecord u;
    u.username = username; u.fullName = trim(fullName); u.role = role;
    u.passwordHash = hasher_.hash(tempPassword);
    u.mustChangePassword = true;
    return users_.insert(u);
}
long long AdminService::createUser(const User& actor, const std::string& username, const std::string& fullName,
                                   Role role, const std::string& tempPassword) {
    requirePermission(actor, Permission::ManageUsers);
    return insertUser(username, fullName, role, tempPassword);
}
void AdminService::deactivate(const User& actor, long long userId) {
    requirePermission(actor, Permission::ManageUsers);
    if (userId == actor.id()) throw BusinessRuleException("You cannot deactivate your own account.");
    requireUser(users_, userId);
    users_.setActive(userId, false);
}
void AdminService::resetPassword(const User& actor, long long userId, const std::string& tempPassword) {
    requirePermission(actor, Permission::ManageUsers);
    UserRecord u = requireUser(users_, userId);
    checkPasswordRule(tempPassword, u.username);
    users_.updatePassword(userId, hasher_.hash(tempPassword), true);
}
long long AdminService::createDepartment(const User& actor, const std::string& name) {
    requirePermission(actor, Permission::ManageCourses);
    requireText(name, "Department name", 100);
    return academic_.insertDepartment(trim(name));
}
void AdminService::setHead(const User& actor, long long departmentId, long long hodUserId) {
    requirePermission(actor, Permission::ManageCourses);
    if (!academic_.findDepartment(departmentId)) throw NotFoundException("Department not found.");
    if (requireUser(users_, hodUserId).role != Role::Hod) throw ValidationException("That user is not a Head of Department.");
    academic_.setHead(departmentId, hodUserId);
}
long long AdminService::createCourse(const User& actor, long long departmentId, const std::string& code, const std::string& title) {
    requirePermission(actor, Permission::ManageCourses);
    if (!academic_.findDepartment(departmentId)) throw NotFoundException("Department not found.");
    requireText(code, "Course code", 30);
    requireText(title, "Course title", 120);
    return academic_.insertCourse(departmentId, trim(code), trim(title));
}
void AdminService::assignLecturer(const User& actor, long long courseId, long long lecturerId) {
    requirePermission(actor, Permission::ManageCourses);
    if (!academic_.findCourse(courseId)) throw NotFoundException("Course not found.");
    Role r = requireUser(users_, lecturerId).role;
    if (r != Role::Lecturer && r != Role::Hod) throw ValidationException("That user is not a lecturer or Head of Department.");
    academic_.addLecturer(courseId, lecturerId);
}
void AdminService::enrolChecked(long long courseId, long long studentId) {
    if (!academic_.findCourse(courseId)) throw NotFoundException("Course not found.");
    if (requireUser(users_, studentId).role != Role::Student) throw ValidationException("That user is not a student.");
    academic_.enrol(courseId, studentId, clock_.now());
}
void AdminService::enrol(const User& actor, long long courseId, long long studentId) {
    requirePermission(actor, Permission::ManageCourses);
    enrolChecked(courseId, studentId);
}
int AdminService::importCsv(const User& actor, const std::string& path) {
    requirePermission(actor, Permission::ManageUsers);
    std::ifstream in(path);
    if (!in) throw ValidationException("Cannot open the file: " + path);
    std::map<std::string, long long> courseIds;
    for (const auto& c : academic_.listCourses()) courseIds[c.code] = c.id;

    Transaction tx(db_);                     // one bad line undoes the whole import
    int count = 0, lineNo = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);   // Excel BOM
        line = trim(line);
        if (line.empty()) continue;
        std::vector<std::string> f;
        std::size_t start = 0, comma;
        while ((comma = line.find(',', start)) != std::string::npos) { f.push_back(trim(line.substr(start, comma - start))); start = comma + 1; }
        f.push_back(trim(line.substr(start)));
        if (lineNo == 1 && f[0] == "username") continue;                // header row
        try {
            if (f.size() < 3 || f.size() > 4) throw ValidationException("expected username,full name,temporary password[,course code]");
            long long id = insertUser(f[0], f[1], Role::Student, f[2]);
            if (f.size() == 4 && !f[3].empty()) {
                auto it = courseIds.find(f[3]);
                if (it == courseIds.end()) throw NotFoundException("course " + f[3] + " does not exist");
                enrolChecked(it->second, id);
            }
            ++count;
        } catch (const AppException& e) {
            throw ValidationException("Import cancelled, nothing was saved. Line " + std::to_string(lineNo) + ": " + e.what());
        }
    }
    tx.commit();
    return count;
}
void AdminService::designateActingHod(const User& actor, long long departmentId, long long lecturerId) {
    requirePermission(actor, Permission::ManageCourses);
    if (!academic_.findDepartment(departmentId)) throw NotFoundException("Department not found.");
    if (requireUser(users_, lecturerId).role != Role::Lecturer) throw ValidationException("The acting head must be a lecturer.");
    Transaction tx(db_);
    academic_.designateActing(departmentId, lecturerId, clock_.now());
    tx.commit();
}
std::vector<UserRecord> AdminService::listUsers(const User& actor) {
    requirePermission(actor, Permission::ManageUsers);
    return users_.list();
}
std::vector<Department> AdminService::listDepartments(const User& actor) {
    requirePermission(actor, Permission::ManageCourses);
    return academic_.listDepartments();
}
std::vector<Course> AdminService::listCourses(const User& actor) {
    requirePermission(actor, Permission::ManageCourses);
    return academic_.listCourses();
}

// ================= AssignmentService =================
Assignment AssignmentService::requireTaught(const User& actor, long long assignmentId) {
    auto a = assignments_.find(assignmentId);
    if (!a) throw NotFoundException("Assignment not found.");
    if (!academic_.teaches(actor.id(), a->courseId)) throw AuthorizationException("You do not teach this course.");
    return *a;
}
long long AssignmentService::create(const User& actor, long long courseId, const std::string& title,
                                    const std::string& description, long long deadline, long long maxMark) {
    requirePermission(actor, Permission::CreateAssignment);
    if (!academic_.findCourse(courseId)) throw NotFoundException("Course not found.");
    if (!academic_.teaches(actor.id(), courseId)) throw AuthorizationException("You do not teach this course.");
    requireText(title, "Title", 120);
    if (description.size() > 2000) throw ValidationException("The instructions are too long (2000 characters at most).");
    if (maxMark < 1 || maxMark > 1000) throw ValidationException("The maximum mark must be from 1 to 1000.");
    Assignment a;
    a.courseId = courseId; a.title = trim(title); a.description = description;
    a.deadline = deadline; a.maxMark = maxMark; a.createdBy = actor.id();
    return assignments_.insert(a);
}
void AssignmentService::close(const User& actor, long long assignmentId) {
    requirePermission(actor, Permission::CreateAssignment);
    requireTaught(actor, assignmentId);
    assignments_.close(assignmentId);
}
void AssignmentService::grantExtension(const User& actor, long long assignmentId, long long studentId,
                                       long long newDeadline, const std::string& reason) {
    requirePermission(actor, Permission::GrantExtension);
    Assignment a = requireTaught(actor, assignmentId);
    if (!academic_.isEnrolled(a.courseId, studentId)) throw ValidationException("That student is not enrolled in this course.");
    requireText(reason, "Reason", 500);
    long long current = assignments_.effectiveDeadline(assignmentId, studentId, a.deadline);
    if (newDeadline <= current) throw BusinessRuleException("The new deadline must be later than the current deadline (" + formatDateTime(current) + ").");
    assignments_.insertExtension(assignmentId, studentId, actor.id(), current, newDeadline, trim(reason), clock_.now());
}
std::vector<AssignmentView> AssignmentService::listForLecturer(const User& actor) {
    requirePermission(actor, Permission::CreateAssignment);
    return assignments_.listForLecturer(actor.id());
}
std::vector<AssignmentView> AssignmentService::listForStudent(const User& actor) {
    requirePermission(actor, Permission::SubmitWork);
    return assignments_.listForStudent(actor.id());
}
std::vector<Course> AssignmentService::coursesForStudent(const User& actor) {
    requirePermission(actor, Permission::SubmitWork);
    return academic_.coursesForStudent(actor.id());
}

std::vector<Course> AssignmentService::coursesForLecturer(const User& actor) {
    requirePermission(actor, Permission::CreateAssignment);
    return academic_.coursesForLecturer(actor.id());
}

// ================= SubmissionService =================
SubmissionPlan SubmissionService::check(const User& student, long long assignmentId, const std::string& sourcePath) {
    requirePermission(student, Permission::SubmitWork);
    auto a = assignments_.find(assignmentId);
    if (!a) throw NotFoundException("Assignment not found.");
    if (!academic_.isEnrolled(a->courseId, student.id())) throw AuthorizationException("You are not enrolled in the course of this assignment.");
    if (a->closed) throw BusinessRuleException("Closed for submissions.");

    std::error_code ec;
    fs::path p(sourcePath);
    if (!fs::is_regular_file(p, ec) || ec) throw ValidationException("The file does not exist or cannot be read.");
    if (fs::file_size(p, ec) == 0 || ec) throw ValidationException("The file is empty.");

    SubmissionPlan plan;
    plan.assignmentId = assignmentId;
    plan.sourcePath = sourcePath;
    plan.now = clock_.now();
    plan.effectiveDeadline = assignments_.effectiveDeadline(assignmentId, student.id(), a->deadline);
    plan.daysLate = DaysLateCalculator::daysLate(plan.now, plan.effectiveDeadline);

    if (auto existing = subs_.findFor(assignmentId, student.id())) {
        SubmissionState::of(existing->state).resubmit();     // throws unless the state is SUBMITTED
        if (plan.now > plan.effectiveDeadline) throw BusinessRuleException("Resubmission refused: the deadline has passed.");
        if (existing->latestVersion >= 5) throw BusinessRuleException("Resubmission refused: you already have 5 versions.");
        plan.isResubmission = true;
        plan.version = existing->latestVersion + 1;
    }
    return plan;
}
SubmissionPlan SubmissionService::prepare(const User& student, long long assignmentId, const std::string& sourcePath) {
    return check(student, assignmentId, sourcePath);
}
Receipt SubmissionService::confirm(const User& student, const SubmissionPlan& asked) {
    Transaction tx(db_);
    SubmissionPlan plan = check(student, asked.assignmentId, asked.sourcePath);   // checked again at the real time
    std::string stored = files_.save(plan.sourcePath, student.id(), plan.assignmentId, plan.version);
    try {
        long long subId;
        if (plan.isResubmission) subId = subs_.findFor(plan.assignmentId, student.id())->id;
        else subId = subs_.insertSubmission(plan.assignmentId, student.id());
        subs_.insertVersion(subId, plan.version, fs::path(plan.sourcePath).filename().string(), stored,
                            static_cast<long long>(fs::file_size(stored)), plan.now);
        tx.commit();
        return {subId, plan.now, plan.version, plan.daysLate};
    } catch (...) {
        files_.remove(stored);
        throw;
    }
}
std::vector<MarkView> SubmissionService::viewMarks(const User& student) {
    requirePermission(student, Permission::ViewOwnMarks);
    std::vector<MarkView> out;
    for (auto d : subs_.listReleasedForStudent(student.id())) {     // FR10: only RELEASED is ever shown
        fillDerived(d, policy_);
        out.push_back({d.courseCode, d.assignmentTitle, d.feedback, d.rawTenths, d.rawTenths - d.finalTenths,
                       d.finalTenths, d.maxMark, d.daysLate});
    }
    return out;
}

// ================= MarkingService =================
std::vector<SubmissionDetail> MarkingService::listSubmissions(const User& actor, long long assignmentId) {
    requirePermission(actor, Permission::MarkSubmission);
    auto a = assignments_.find(assignmentId);
    if (!a) throw NotFoundException("Assignment not found.");
    if (!academic_.teaches(actor.id(), a->courseId)) throw AuthorizationException("You do not teach this course.");
    auto list = subs_.listForAssignment(assignmentId);
    for (auto& d : list) fillDerived(d, policy_);
    return list;
}
std::vector<SubmissionDetail> MarkingService::listUnmarked(const User& actor) {
    requirePermission(actor, Permission::MarkSubmission);
    auto list = subs_.listForLecturer(actor.id(), State::Submitted);
    for (auto& d : list) fillDerived(d, policy_);
    return list;
}
void MarkingService::mark(const User& actor, long long submissionId, long long rawTenths,
                          const std::string& feedback, const std::string& reason) {
    requirePermission(actor, Permission::MarkSubmission);
    Transaction tx(db_);
    auto d = subs_.find(submissionId);
    if (!d) throw NotFoundException("Submission not found.");
    if (!academic_.teaches(actor.id(), d->courseId)) throw AuthorizationException("You do not teach this course.");
    if (d->state == State::Moderated || d->state == State::Released)
        throw BusinessRuleException("Only the Head of Department can change this mark.");
    if (rawTenths < 0 || rawTenths > d->maxMark * 10) throw ValidationException("The mark is out of range.");
    requireText(feedback, "Feedback", 2000);

    bool changing = d->state != State::Submitted;           // MARKED (revise) or RETURNED (re-mark)
    if (changing) requireText(reason, "The reason for the change", 500);
    const SubmissionState& st = SubmissionState::of(d->state);
    State next = d->state == State::Marked ? st.revise() : st.mark();

    fillDerived(*d, policy_);
    long long oldFinal = d->finalTenths;
    if (!subs_.saveMark(submissionId, d->state, next, rawTenths, feedback, actor.id()))
        throw BusinessRuleException("The submission changed while you were working. Please try again.");
    AuditEntry e;
    e.submissionId = submissionId; e.changedBy = actor.id(); e.changedAt = clock_.now();
    e.action = changing ? "REVISE" : "MARK";
    e.hasOld = d->hasMark; e.oldRaw = d->rawTenths; e.oldFinal = oldFinal;
    e.newRaw = rawTenths; e.newFinal = policy_.finalTenths(rawTenths, d->maxMark, d->daysLate);
    e.reason = changing ? trim(reason) : "Initial mark";
    audit_.record(e);
    tx.commit();
}

// ================= ModerationService =================
std::vector<long long> ModerationService::departmentsFor(const User& actor) {
    std::vector<long long> depts;
    if (actor.getRole() == Role::Hod) depts = academic_.headedDepartments(actor.id());
    if (actor.getRole() == Role::Lecturer)
        for (long long d : academic_.actingDepartments(actor.id())) depts.push_back(d);
    return depts;
}
ModerationService::Checked ModerationService::load(const User& actor, long long submissionId) {
    auto depts = departmentsFor(actor);
    if (depts.empty()) throw AuthorizationException("You are not a head or acting head of a department.");
    auto d = subs_.find(submissionId);
    if (!d) throw NotFoundException("Submission not found.");
    if (!contains(depts, d->departmentId)) throw AuthorizationException("This submission belongs to another department.");
    if (academic_.teaches(actor.id(), d->courseId))
        throw AuthorizationException("You teach this course, so another head must moderate it.");
    fillDerived(*d, policy_);
    return {*d, actor.getRole() == Role::Lecturer};
}
void ModerationService::record(const User& actor, const Checked& c, const char* action, bool hasOld,
                               long long oldRaw, long long newRaw, const std::string& reason) {
    AuditEntry e;
    e.submissionId = c.detail.id; e.changedBy = actor.id(); e.changedAt = clock_.now(); e.action = action;
    e.hasOld = hasOld; e.oldRaw = oldRaw; e.newRaw = newRaw;
    e.oldFinal = policy_.finalTenths(oldRaw, c.detail.maxMark, c.detail.daysLate);
    e.newFinal = policy_.finalTenths(newRaw, c.detail.maxMark, c.detail.daysLate);
    e.reason = reason; e.actingCapacity = c.acting;
    audit_.record(e);
}
std::vector<SubmissionDetail> ModerationService::queue(const User& actor) {
    auto depts = departmentsFor(actor);
    if (depts.empty()) throw AuthorizationException("You are not a head or acting head of a department.");
    std::vector<SubmissionDetail> out;
    for (long long dept : depts)
        for (auto& d : subs_.listForDepartment(dept, State::Marked))
            if (!academic_.teaches(actor.id(), d.courseId)) { fillDerived(d, policy_); out.push_back(d); }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.latestSubmittedAt < b.latestSubmittedAt; });
    return out;
}
std::vector<SubmissionDetail> ModerationService::approvedQueue(const User& actor) {
    auto depts = departmentsFor(actor);
    if (depts.empty()) throw AuthorizationException("You are not a head or acting head of a department.");
    std::vector<SubmissionDetail> out;
    for (long long dept : depts)
        for (auto& d : subs_.listForDepartment(dept, State::Moderated))
            if (!academic_.teaches(actor.id(), d.courseId)) { fillDerived(d, policy_); out.push_back(d); }
    return out;
}
void ModerationService::approve(const User& actor, long long submissionId, const std::string& comment) {
    Transaction tx(db_);
    Checked c = load(actor, submissionId);
    State next = SubmissionState::of(c.detail.state).approve();
    if (!subs_.saveApproval(submissionId, c.detail.state, next, actor.id(), trim(comment)))
        throw BusinessRuleException("The submission changed since the list was loaded.");
    record(actor, c, "APPROVE", true, c.detail.rawTenths, c.detail.rawTenths, trim(comment).empty() ? "Approved" : trim(comment));
    tx.commit();
}
void ModerationService::adjust(const User& actor, long long submissionId, long long newRawTenths, const std::string& reason) {
    Transaction tx(db_);
    Checked c = load(actor, submissionId);
    requireText(reason, "The reason", 500);
    if (newRawTenths < 0 || newRawTenths > c.detail.maxMark * 10) throw ValidationException("The mark is out of range.");
    State next = SubmissionState::of(c.detail.state).adjust();
    if (!subs_.saveAdjustment(submissionId, c.detail.state, next, newRawTenths, actor.id(), trim(reason)))
        throw BusinessRuleException("The submission changed since the list was loaded.");
    record(actor, c, "ADJUST", true, c.detail.rawTenths, newRawTenths, trim(reason));
    tx.commit();
}
void ModerationService::returnToLecturer(const User& actor, long long submissionId, const std::string& comment) {
    Transaction tx(db_);
    Checked c = load(actor, submissionId);
    requireText(comment, "The comment", 500);
    State next = SubmissionState::of(c.detail.state).returnToLecturer();
    if (!subs_.saveReturn(submissionId, c.detail.state, next, trim(comment)))
        throw BusinessRuleException("The submission changed since the list was loaded.");
    record(actor, c, "RETURN", true, c.detail.rawTenths, c.detail.rawTenths, trim(comment));
    tx.commit();
}
int ModerationService::release(const User& actor, const std::vector<long long>& submissionIds) {
    if (submissionIds.empty()) throw ValidationException("Nothing was selected.");
    Transaction tx(db_);                               // any failure below rolls back every release
    for (long long id : submissionIds) {
        Checked c = load(actor, id);
        State next = SubmissionState::of(c.detail.state).release();
        if (!subs_.saveRelease(id, c.detail.state, next, clock_.now()))
            throw BusinessRuleException("Submission " + std::to_string(id) + " changed. Nothing was released.");
        record(actor, c, "RELEASE", true, c.detail.rawTenths, c.detail.rawTenths, "Released to student");
    }
    tx.commit();
    return static_cast<int>(submissionIds.size());
}
void ModerationService::amend(const User& actor, long long submissionId, long long newRawTenths, const std::string& reason) {
    Transaction tx(db_);
    Checked c = load(actor, submissionId);
    requireText(reason, "The reason", 500);
    if (newRawTenths < 0 || newRawTenths > c.detail.maxMark * 10) throw ValidationException("The mark is out of range.");
    SubmissionState::of(c.detail.state).amend();       // throws unless RELEASED
    if (!subs_.saveAmendment(submissionId, c.detail.state, newRawTenths))
        throw BusinessRuleException("The submission changed since the list was loaded.");
    record(actor, c, "AMEND", true, c.detail.rawTenths, newRawTenths, trim(reason));
    tx.commit();
}

// ================= BackupService =================
void BackupService::backup(const User& actor, const std::string& path) {
    requirePermission(actor, Permission::Backup);
    requireText(path, "The backup path", 500);
    if (fs::exists(path)) throw ValidationException("That file already exists. Choose a new file name.");
    db_.backupTo(path);
}
