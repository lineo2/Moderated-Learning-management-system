// Simple self-contained tests (no test library to install). Run with: ctest --test-dir build --output-on-failure
// Services are tested against an in-memory SQLite database with a fake clock, so they run in about a second.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>

#include "Console.h"

namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::cout << "  FAIL line " << __LINE__ << ": " #cond "\n"; } } while (0)
#define CHECK_THROWS(expr, Type) do { ++g_checks; bool ok_ = false; \
    try { expr; } catch (const Type&) { ok_ = true; } catch (...) {} \
    if (!ok_) { ++g_failed; std::cout << "  FAIL line " << __LINE__ << ": expected " #Type " from " #expr "\n"; } } while (0)

struct FakeClock : IClock {
    long long t = 1800000000;                 // a fixed moment, so results never depend on today's date
    long long now() const override { return t; }
};
constexpr long long DAY = 86400;

// Everything wired up like main(), but with an in-memory database.
struct World {
    FakeClock clock;
    Argon2Hasher hasher{true};
    fs::path dir = fs::temp_directory_path() / ("mlms_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LocalFileStore files{dir / "submissions"};
    TenPercentPerDayPolicy policy;
    Database db{":memory:"};
    UserRepository users{db};
    AcademicRepository academic{db};
    AssignmentRepository assignmentRepo{db};
    SubmissionRepository subRepo{db};
    AuditRepository auditRepo{db};
    AuditService audit{auditRepo};
    AuthService auth{users, hasher, clock};
    AdminService admin{db, users, academic, hasher, clock};
    AssignmentService assignments{assignmentRepo, academic, clock};
    SubmissionService submissions{db, subRepo, assignmentRepo, academic, files, clock, policy};
    MarkingService marking{db, subRepo, assignmentRepo, academic, audit, policy, clock};
    ModerationService moderation{db, subRepo, academic, audit, policy, clock};
    BackupService backup{db};

    std::unique_ptr<User> adminUser, lecturer, lecturer2, hod, hod2, student, student2;
    long long deptId = 0, dept2Id = 0, courseId = 0, course2Id = 0;

    std::string makeFile(const std::string& name, const std::string& content) {
        fs::create_directories(dir);
        fs::path p = dir / name;
        std::ofstream(p) << content;
        return p.string();
    }
    std::unique_ptr<User> addUser(const std::string& name, Role role) {
        long long id = admin.createUser(*adminUser, name, name + " Person", role, "password123");
        return UserFactory::create(*users.findById(id));
    }
    World() {
        admin.createFirstAdminIfNeeded("ChangeMe123!");
        adminUser = UserFactory::create(*users.findByUsername("admin"));
        lecturer = addUser("lec1", Role::Lecturer);
        lecturer2 = addUser("lec2", Role::Lecturer);
        hod = addUser("hod1", Role::Hod);
        hod2 = addUser("hod2", Role::Hod);
        student = addUser("stu1", Role::Student);
        student2 = addUser("stu2", Role::Student);
        deptId = admin.createDepartment(*adminUser, "Science");
        dept2Id = admin.createDepartment(*adminUser, "Arts");
        admin.setHead(*adminUser, deptId, hod->id());
        admin.setHead(*adminUser, dept2Id, hod2->id());
        courseId = admin.createCourse(*adminUser, deptId, "SCI101", "Science 1");
        course2Id = admin.createCourse(*adminUser, deptId, "SCI102", "Science 2");
        admin.assignLecturer(*adminUser, courseId, lecturer->id());
        admin.assignLecturer(*adminUser, course2Id, lecturer2->id());
        admin.enrol(*adminUser, courseId, student->id());
        admin.enrol(*adminUser, courseId, student2->id());
    }
    ~World() { std::error_code ec; fs::remove_all(dir, ec); }

    // assignment due `daysFromNow` days after the fake clock, max mark 100
    long long newAssignment(const std::string& title, long long daysFromNow) {
        return assignments.create(*lecturer, courseId, title, "", clock.t + daysFromNow * DAY, 100);
    }
    long long submit(const User& who, long long assignmentId, const std::string& content = "my work") {
        auto plan = submissions.prepare(who, assignmentId, makeFile("work.txt", content));
        return submissions.confirm(who, plan).submissionId;
    }
};

// ---------------------------------------------------------------- pure domain rules
static void testPenalty() {
    TenPercentPerDayPolicy p;                                   // marks in tenths, max mark 100
    CHECK(p.finalTenths(805, 100, 0) == 805);                   // on time
    CHECK(p.finalTenths(805, 100, 1) == 705);                   // minus 10 marks
    CHECK(p.finalTenths(805, 100, 2) == 605);
    CHECK(p.finalTenths(805, 100, 3) == 505);                   // minus 30 marks
    CHECK(p.finalTenths(805, 100, 4) == 0);                     // four or more days: zero
    CHECK(p.finalTenths(805, 100, 40) == 0);
    CHECK(p.finalTenths(200, 100, 3) == 0);                     // never below zero
    CHECK(p.finalTenths(500, 50, 2) == 400);                    // 10% of max 50 is 5 marks per day
    NoPenaltyPolicy n;
    CHECK(n.finalTenths(805, 100, 3) == 805);                   // the strategy can be swapped
}
static void testDaysLate() {
    CHECK(DaysLateCalculator::daysLate(1000, 1000) == 0);
    CHECK(DaysLateCalculator::daysLate(900, 1000) == 0);
    CHECK(DaysLateCalculator::daysLate(1001, 1000) == 1);
    CHECK(DaysLateCalculator::daysLate(1000 + DAY, 1000) == 1);
    CHECK(DaysLateCalculator::daysLate(1001 + DAY, 1000) == 2);
    CHECK(DaysLateCalculator::daysLate(1000 + 3 * DAY, 1000) == 3);
    CHECK(DaysLateCalculator::daysLate(1001 + 3 * DAY, 1000) == 4);
}
static void testMarkParsing() {
    CHECK(parseMarkTenths("40.5", 100) == 405);
    CHECK(parseMarkTenths("0", 100) == 0);
    CHECK(parseMarkTenths("100", 100) == 1000);
    CHECK_THROWS(parseMarkTenths("100.1", 100), ValidationException);
    CHECK_THROWS(parseMarkTenths("40.55", 100), ValidationException);
    CHECK_THROWS(parseMarkTenths("-5", 100), ValidationException);
    CHECK_THROWS(parseMarkTenths("abc", 100), ValidationException);
    CHECK_THROWS(parseMarkTenths("", 100), ValidationException);
    CHECK(markText(405) == "40.5");
    CHECK(markText(1000) == "100.0");
}
static void testDatesAndPasswordRule() {
    CHECK(formatDateTime(parseDateTime("2026-11-05 17:00")) == "2026-11-05 17:00");
    CHECK_THROWS(parseDateTime("2026-13-05 17:00"), ValidationException);
    CHECK_THROWS(parseDateTime("tomorrow"), ValidationException);
    CHECK_THROWS(checkPasswordRule("short", "bob"), ValidationException);
    CHECK_THROWS(checkPasswordRule("samename1", "samename1"), ValidationException);
    checkPasswordRule("longenough", "bob");
}
static void testStateMachine() {
    CHECK(SubmissionState::of(State::Submitted).mark() == State::Marked);
    CHECK(SubmissionState::of(State::Marked).approve() == State::Moderated);
    CHECK(SubmissionState::of(State::Marked).returnToLecturer() == State::Returned);
    CHECK(SubmissionState::of(State::Returned).mark() == State::Marked);
    CHECK(SubmissionState::of(State::Moderated).release() == State::Released);
    CHECK(SubmissionState::of(State::Released).amend() == State::Released);
    CHECK_THROWS(SubmissionState::of(State::Submitted).release(), IllegalTransitionException);   // cannot release unmarked work
    CHECK_THROWS(SubmissionState::of(State::Marked).release(), IllegalTransitionException);
    CHECK_THROWS(SubmissionState::of(State::Released).approve(), IllegalTransitionException);    // approve cannot un-release
    CHECK_THROWS(SubmissionState::of(State::Marked).resubmit(), IllegalTransitionException);
    CHECK_THROWS(SubmissionState::of(State::Moderated).mark(), IllegalTransitionException);
}
static void testHasher() {
    Argon2Hasher h(true);
    std::string a = h.hash("secret-pass"), b = h.hash("secret-pass");
    CHECK(a != "secret-pass");
    CHECK(a != b);                                              // random salt
    CHECK(h.verify("secret-pass", a));
    CHECK(!h.verify("wrong-pass", a));
}

// ---------------------------------------------------------------- services
static void testLogin() {
    World w;
    Session s = w.auth.login("LEC1", "password123");            // user names ignore case
    CHECK(s.user->getRole() == Role::Lecturer);
    CHECK(s.user->mustChangePassword());                        // temporary password
    CHECK_THROWS(w.auth.login("lec1", "wrong"), AuthorizationException);
    CHECK_THROWS(w.auth.login("nobody", "password123"), AuthorizationException);
    auto hash = w.users.findByUsername("lec1")->passwordHash;
    CHECK(hash.find("password123") == std::string::npos);       // only a hash is stored

    CHECK_THROWS(w.auth.changePassword(s, "short"), ValidationException);
    w.auth.changePassword(s, "a-new-password");
    CHECK(!w.users.findByUsername("lec1")->mustChangePassword);
    w.auth.login("lec1", "a-new-password");

    w.admin.deactivate(*w.adminUser, w.lecturer->id());
    CHECK_THROWS(w.auth.login("lec1", "a-new-password"), AuthorizationException);
    CHECK_THROWS(w.admin.deactivate(*w.adminUser, w.adminUser->id()), BusinessRuleException);
    w.admin.resetPassword(*w.adminUser, w.lecturer->id(), "temp-pass-1");
    CHECK(w.users.findById(w.lecturer->id())->mustChangePassword);
}
static void testIdleTimeout() {
    World w;
    Session s = w.auth.login("stu1", "password123");
    w.clock.t += 9 * 60;
    w.auth.checkIdle(s);                                        // still fine
    w.auth.touch(s);
    w.clock.t += 11 * 60;
    CHECK_THROWS(w.auth.checkIdle(s), SessionExpiredException);
    CHECK(s.user == nullptr);                                   // logged out
}
static void testRoleChecks() {
    World w;
    CHECK_THROWS(w.admin.createUser(*w.student, "x1", "X", Role::Student, "password123"), AuthorizationException);
    CHECK_THROWS(w.admin.createDepartment(*w.lecturer, "Hack"), AuthorizationException);
    CHECK_THROWS(w.assignments.create(*w.student, w.courseId, "t", "", w.clock.t + DAY, 10), AuthorizationException);
    long long a = w.newAssignment("A1", 1);
    CHECK_THROWS(w.submissions.prepare(*w.lecturer, a, w.makeFile("f.txt", "x")), AuthorizationException);
    CHECK_THROWS(w.audit.view(*w.student), AuthorizationException);
    CHECK_THROWS(w.backup.backup(*w.lecturer, (w.dir / "b.db").string()), AuthorizationException);
    // lecturer 2 does not teach course 1
    CHECK_THROWS(w.assignments.create(*w.lecturer2, w.courseId, "t", "", w.clock.t + DAY, 10), AuthorizationException);
    long long sid = w.submit(*w.student, a);
    CHECK_THROWS(w.marking.mark(*w.lecturer2, sid, 500, "ok", ""), AuthorizationException);
    CHECK_THROWS(w.marking.mark(*w.student, sid, 500, "ok", ""), AuthorizationException);
    CHECK_THROWS(w.moderation.queue(*w.student), AuthorizationException);
}
static void testSubmissionRules() {
    World w;
    long long a = w.newAssignment("A1", 1);
    CHECK_THROWS(w.submissions.prepare(*w.student, a, (w.dir / "missing.txt").string()), ValidationException);
    CHECK_THROWS(w.submissions.prepare(*w.student, a, w.makeFile("empty.txt", "")), ValidationException);
    auto stranger = w.addUser("stu3", Role::Student);           // not enrolled
    CHECK_THROWS(w.submissions.prepare(*stranger, a, w.makeFile("f.txt", "x")), AuthorizationException);

    for (int v = 1; v <= 5; ++v) {                              // five versions allowed
        auto plan = w.submissions.prepare(*w.student, a, w.makeFile("w.txt", "v" + std::to_string(v)));
        CHECK(plan.version == v);
        CHECK(plan.isResubmission == (v > 1));
        CHECK(w.submissions.confirm(*w.student, plan).version == v);
    }
    CHECK_THROWS(w.submissions.prepare(*w.student, a, w.makeFile("w.txt", "v6")), BusinessRuleException);   // sixth refused
    CHECK(w.subRepo.versionCount(w.subRepo.findFor(a, w.student->id())->id) == 5);

    long long b = w.newAssignment("A2", 1);                     // closing blocks new work
    w.assignments.close(*w.lecturer, b);
    CHECK_THROWS(w.submissions.prepare(*w.student, b, w.makeFile("f.txt", "x")), BusinessRuleException);
}
static void testLateAndExtension() {
    World w;
    long long a = w.newAssignment("A1", 1);                     // due in one day
    w.clock.t += 1 * DAY + 3600;                                // one hour late -> 1 day late
    auto plan = w.submissions.prepare(*w.student, a, w.makeFile("f.txt", "late"));
    CHECK(plan.daysLate == 1);
    long long sid = w.submissions.confirm(*w.student, plan).submissionId;
    CHECK_THROWS(w.submissions.prepare(*w.student, a, w.makeFile("g.txt", "again")), BusinessRuleException);  // no resubmit after deadline

    w.marking.mark(*w.lecturer, sid, 800, "Good work", "");     // 80.0 of 100
    auto d = w.marking.listSubmissions(*w.lecturer, a);
    CHECK(d.size() == 1 && d[0].daysLate == 1 && d[0].finalTenths == 700);   // 80 - 10 = 70

    // extension: the penalty is measured from the extended deadline
    long long b = w.newAssignment("A2", 0);                     // due now
    w.clock.t += 3 * DAY;                                       // 3 days later
    long long ext = w.clock.t - 2 * DAY;                        // extended to 1 day ago: now 2 days late
    w.assignments.grantExtension(*w.lecturer, b, w.student2->id(), ext, "illness");
    CHECK_THROWS(w.assignments.grantExtension(*w.lecturer, b, w.student2->id(), ext, "again"), BusinessRuleException);  // must be later
    CHECK_THROWS(w.assignments.grantExtension(*w.lecturer, b, w.student2->id(), ext + DAY, ""), ValidationException);   // reason needed
    CHECK(w.submissions.prepare(*w.student2, b, w.makeFile("h.txt", "x")).daysLate == 2);
    CHECK(w.submissions.prepare(*w.student, b, w.makeFile("h.txt", "x")).daysLate == 3);      // no extension: 3 days late

    long long c = w.newAssignment("A3", -10);                   // 10 days overdue: accepted, final mark zero
    long long s3 = w.submit(*w.student, c);
    w.marking.mark(*w.lecturer, s3, 900, "Late", "");
    CHECK(w.marking.listSubmissions(*w.lecturer, c)[0].finalTenths == 0);
}
static void testFullWorkflowAndVisibility() {
    World w;
    long long a = w.newAssignment("A1", 5);
    long long sid = w.submit(*w.student, a);
    CHECK(w.submissions.viewMarks(*w.student).empty());
    CHECK(w.marking.listUnmarked(*w.lecturer).size() == 1);     // dashboard
    CHECK_THROWS(w.moderation.approve(*w.hod, sid, ""), IllegalTransitionException);   // not marked yet
    CHECK_THROWS(w.moderation.release(*w.hod, {sid}), IllegalTransitionException);

    w.marking.mark(*w.lecturer, sid, 755, "Nice", "");          // 75.5
    CHECK(w.marking.listUnmarked(*w.lecturer).empty());
    CHECK(w.submissions.viewMarks(*w.student).empty());         // FR10: hidden while MARKED
    CHECK(w.moderation.queue(*w.hod).size() == 1);

    w.marking.mark(*w.lecturer, sid, 765, "Better", "typo in my addition");    // revise needs a reason
    CHECK_THROWS(w.marking.mark(*w.lecturer, sid, 770, "x", ""), ValidationException);

    w.moderation.returnToLecturer(*w.hod, sid, "Check feedback");
    CHECK(w.subRepo.find(sid)->state == State::Returned);
    CHECK(w.moderation.queue(*w.hod).empty());
    w.marking.mark(*w.lecturer, sid, 770, "Rewritten feedback", "HoD asked");  // re-mark after return
    w.moderation.adjust(*w.hod, sid, 780, "Marking guide");
    CHECK(w.subRepo.find(sid)->state == State::Moderated);
    CHECK(w.submissions.viewMarks(*w.student).empty());         // still hidden while MODERATED
    CHECK_THROWS(w.marking.mark(*w.lecturer, sid, 700, "x", "y"), BusinessRuleException);   // only HoD may change it now

    CHECK(w.moderation.approvedQueue(*w.hod).size() == 1);
    CHECK(w.moderation.release(*w.hod, {sid}) == 1);
    auto marks = w.submissions.viewMarks(*w.student);
    CHECK(marks.size() == 1 && marks[0].finalTenths == 780 && marks[0].feedback == "Rewritten feedback");

    CHECK_THROWS(w.moderation.approve(*w.hod, sid, ""), IllegalTransitionException);   // cannot un-release
    w.moderation.amend(*w.hod, sid, 790, "Addition error found");
    CHECK(w.submissions.viewMarks(*w.student)[0].finalTenths == 790);
    CHECK(w.subRepo.find(sid)->state == State::Released);
    CHECK_THROWS(w.moderation.amend(*w.hod, sid, 790, ""), ValidationException);       // reason needed

    // every change is in the audit log, oldest to newest
    auto log = w.audit.view(*w.adminUser);
    std::vector<std::string> actions;
    for (auto it = log.rbegin(); it != log.rend(); ++it) actions.push_back(it->action);
    std::vector<std::string> expected = {"MARK", "REVISE", "RETURN", "REVISE", "ADJUST", "RELEASE", "AMEND"};
    CHECK(actions == expected);
    CHECK(log.front().oldRaw == 780 && log.front().newRaw == 790);
}
static void testModerationIndependence() {
    World w;
    long long a = w.newAssignment("A1", 5);
    long long sid = w.submit(*w.student, a);
    w.marking.mark(*w.lecturer, sid, 600, "ok", "");
    CHECK_THROWS(w.moderation.approve(*w.hod2, sid, ""), AuthorizationException);       // other department
    CHECK_THROWS(w.moderation.approve(*w.lecturer, sid, ""), AuthorizationException);   // plain lecturer
    // The HoD of Science also teaches SCI101: independence rule blocks moderating it.
    w.admin.assignLecturer(*w.adminUser, w.courseId, w.hod->id());
    CHECK_THROWS(w.moderation.approve(*w.hod, sid, ""), AuthorizationException);
    CHECK(w.moderation.queue(*w.hod).empty());
    // The database also enforces it (trigger), even if a service check were missing.
    CHECK_THROWS(w.db.execute("UPDATE submission SET state='MODERATED', moderated_by=? WHERE submission_id=?",
                              {Value(w.hod->id()), Value(sid)}), DatabaseException);
}
static void testActingHod() {
    World w;
    long long a = w.newAssignment("A1", 5);
    long long sid = w.submit(*w.student, a);
    w.marking.mark(*w.lecturer, sid, 600, "ok", "");
    CHECK(w.moderation.departmentsFor(*w.lecturer2).empty());
    CHECK_THROWS(w.moderation.approve(*w.lecturer2, sid, ""), AuthorizationException);
    w.admin.designateActingHod(*w.adminUser, w.deptId, w.lecturer2->id());               // lec2 does not teach SCI101
    CHECK(w.moderation.departmentsFor(*w.lecturer2).size() == 1);
    w.moderation.approve(*w.lecturer2, sid, "ok");
    CHECK(w.audit.view(*w.adminUser).front().actingCapacity);                            // audit says "acting"
    w.clock.t += 10;
    w.admin.designateActingHod(*w.adminUser, w.deptId, w.lecturer->id());                // replaces lec2
    CHECK(w.moderation.departmentsFor(*w.lecturer2).empty());
}
static void testAtomicRelease() {
    World w;
    std::vector<long long> ids;
    for (const char* t : {"A1", "A2"}) {
        long long a = w.newAssignment(t, 5);
        long long sid = w.submit(*w.student, a);
        w.marking.mark(*w.lecturer, sid, 600, "ok", "");
        w.moderation.approve(*w.hod, sid, "");
        ids.push_back(sid);
    }
    long long a3 = w.newAssignment("A3", 5);
    long long unmarked = w.submit(*w.student, a3);                                       // never marked
    CHECK_THROWS(w.moderation.release(*w.hod, {ids[0], ids[1], unmarked}), AppException);
    CHECK(w.subRepo.find(ids[0])->state == State::Moderated);                            // all or nothing
    CHECK(w.subRepo.find(ids[1])->state == State::Moderated);
    CHECK(w.submissions.viewMarks(*w.student).empty());
    CHECK(w.moderation.release(*w.hod, {ids[0], ids[1]}) == 2);
    CHECK(w.submissions.viewMarks(*w.student).size() == 2);
}
static void testAuditAppendOnly() {
    World w;
    long long a = w.newAssignment("A1", 5);
    long long sid = w.submit(*w.student, a);
    w.marking.mark(*w.lecturer, sid, 600, "ok", "");
    CHECK_THROWS(w.db.execute("UPDATE audit_entry SET reason='hacked'"), DatabaseException);
    CHECK_THROWS(w.db.execute("DELETE FROM audit_entry"), DatabaseException);
    CHECK(w.audit.view(*w.adminUser).size() == 1);
    // Mark above maximum is refused by the service and by the database.
    long long b = w.newAssignment("A2", 5);
    long long s2 = w.submit(*w.student, b);
    CHECK_THROWS(w.marking.mark(*w.lecturer, s2, 1001, "x", ""), ValidationException);
    CHECK_THROWS(w.db.execute("UPDATE submission SET raw_tenths=2000 WHERE submission_id=?", {Value(s2)}), DatabaseException);
    // SQL injection text is only ever data.
    long long id = w.admin.createUser(*w.adminUser, "x');DROP-TABLE-app_user;--", "Evil", Role::Student, "password123");
    CHECK(id > 0 && w.users.list().size() > 5);
}
static void testCsvImport() {
    World w;
    std::string good = w.makeFile("good.csv", "username,full name,temporary password,course\n"
                                              "s100,Ann One,temp-pass-1,SCI101\ns101,Bob Two,temp-pass-2\n");
    CHECK(w.admin.importCsv(*w.adminUser, good) == 2);
    CHECK(w.users.findByUsername("s100").has_value());
    CHECK(w.academic.isEnrolled(w.courseId, w.users.findByUsername("s100")->id));
    std::string bad = w.makeFile("bad.csv", "s200,Cat Three,temp-pass-3\ns201,Dan Four,short\n");
    CHECK_THROWS(w.admin.importCsv(*w.adminUser, bad), ValidationException);
    CHECK(!w.users.findByUsername("s200").has_value());                                  // whole import undone
    CHECK_THROWS(w.admin.importCsv(*w.adminUser, (w.dir / "nope.csv").string()), ValidationException);
}
static void testBackup() {
    World w;
    std::string path = (w.dir / "backup.db").string();
    w.backup.backup(*w.adminUser, path);
    Database copy(path);
    CHECK(copy.query("SELECT COUNT(*) FROM app_user")[0].num(0) == 7);
    CHECK_THROWS(w.backup.backup(*w.adminUser, path), ValidationException);              // never overwrites
}
static void testCapacity() {                                                              // NFR3 at full size
    World w;
    w.db.execute("BEGIN");
    for (int i = 0; i < 800; ++i)
        w.db.execute("INSERT INTO app_user(username,full_name,role,password_hash) VALUES(?,?,'STUDENT','x')",
                     {Value("load" + std::to_string(i)), Value(std::string("Load Student"))});
    w.db.execute("COMMIT");
    long long a = w.newAssignment("A1", 5);
    w.db.execute("BEGIN");
    for (int i = 1; i <= 800; ++i) w.db.execute("INSERT INTO submission(assignment_id,student_id) VALUES(?,?)", {Value(a), Value(static_cast<long long>(i + 7))});
    w.db.execute("COMMIT");
    CHECK(w.marking.listSubmissions(*w.lecturer, a).size() == 800);
    CHECK(w.users.findByUsername("load799").has_value());
}

// ---------------------------------------------------------------- console
static void testConsoleScript() {
    World w;
    Services sv{w.auth, w.admin, w.assignments, w.submissions, w.marking, w.moderation, w.audit, w.backup};
    // first login of the seeded admin must force a password change, then create a department, then log out and quit
    std::istringstream in("admin\nChangeMe123!\nnewpass-123\nnewpass-123\n5\nMaths\n0\nq\n");
    std::ostringstream out;
    Io io(in, out);
    ConsoleApp(io, sv).run();
    CHECK(out.str().find("You must choose your own password") != std::string::npos);
    CHECK(out.str().find("Department created") != std::string::npos);
    CHECK(out.str().find("Goodbye") != std::string::npos);
    w.auth.login("admin", "newpass-123");

    std::istringstream cut("admin\nnewpass-123\n");           // input ends in the middle of a session: must not hang
    std::ostringstream out2;
    Io io2(cut, out2);
    ConsoleApp(io2, sv).run();
    CHECK(out2.str().find("Goodbye") != std::string::npos);

    std::istringstream bad("admin\nwrong\nq\n");              // wrong password is reported, no crash
    std::ostringstream out3;
    Io io3(bad, out3);
    ConsoleApp(io3, sv).run();
    CHECK(out3.str().find("Login failed") != std::string::npos);
}

int main() {
    struct T { const char* name; std::function<void()> fn; };
    std::vector<T> tests = {
        {"late penalty boundaries", testPenalty}, {"days late", testDaysLate}, {"mark parsing", testMarkParsing},
        {"dates and password rule", testDatesAndPasswordRule}, {"submission state machine", testStateMachine},
        {"argon2 hasher", testHasher}, {"login and passwords", testLogin}, {"idle timeout", testIdleTimeout},
        {"role-based access", testRoleChecks}, {"submission rules", testSubmissionRules},
        {"late work and extensions", testLateAndExtension}, {"full workflow and visibility", testFullWorkflowAndVisibility},
        {"moderation independence", testModerationIndependence}, {"acting head of department", testActingHod},
        {"atomic release", testAtomicRelease}, {"audit append-only and constraints", testAuditAppendOnly},
        {"csv import", testCsvImport}, {"backup", testBackup}, {"capacity (800 students)", testCapacity},
        {"console script", testConsoleScript},
    };
    for (auto& t : tests) {
        int before = g_failed;
        try { t.fn(); } catch (const std::exception& e) { ++g_failed; std::cout << "  EXCEPTION: " << e.what() << "\n"; }
        std::cout << (g_failed == before ? "PASS  " : "FAIL  ") << t.name << "\n";
    }
    std::cout << "\n" << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
