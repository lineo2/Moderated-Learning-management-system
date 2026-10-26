#include "Console.h"

#include <functional>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

// ================= Io =================
std::string Io::ask(const std::string& prompt) {
    out_ << prompt << std::flush;
    std::string line;
    if (!std::getline(in_, line)) throw EndOfInput();          // never loop forever on a closed input
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}
std::string Io::required(const std::string& prompt) {
    while (true) {
        std::string v = ask(prompt);
        if (!trim(v).empty()) return v;
        out_ << "This cannot be empty.\n";
    }
}
long long Io::number(const std::string& prompt, long long lo, long long hi) {
    while (true) {
        std::string v = trim(ask(prompt));
        try {
            std::size_t used = 0;
            long long n = std::stoll(v, &used);
            if (used == v.size() && n >= lo && n <= hi) return n;
        } catch (...) {}
        out_ << "Please enter a whole number from " << lo << " to " << hi << ".\n";
    }
}
bool Io::yesNo(const std::string& prompt) {
    while (true) {
        std::string v = trim(ask(prompt + " (y/n): "));
        if (v == "y" || v == "Y") return true;
        if (v == "n" || v == "N") return false;
        out_ << "Please type y or n.\n";
    }
}
std::string Io::secret(const std::string& prompt) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool hide = &in_ == &std::cin && GetConsoleMode(h, &mode);
    if (hide) SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
    try { std::string v = ask(prompt); if (hide) { SetConsoleMode(h, mode); out_ << "\n"; } return v; }
    catch (...) { if (hide) SetConsoleMode(h, mode); throw; }
#else
    termios old{};
    bool hide = &in_ == &std::cin && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (hide) { termios t = old; t.c_lflag &= ~static_cast<tcflag_t>(ECHO); tcsetattr(STDIN_FILENO, TCSANOW, &t); }
    try { std::string v = ask(prompt); if (hide) { tcsetattr(STDIN_FILENO, TCSANOW, &old); out_ << "\n"; } return v; }
    catch (...) { if (hide) tcsetattr(STDIN_FILENO, TCSANOW, &old); throw; }
#endif
}

namespace {

struct Item { std::string label; std::function<void()> action; };
constexpr long long kMaxId = 1000000000;

// Shared by the four role menus.
class MenuBase {
public:
    MenuBase(Io& io, Services& sv, Session& s) : io(io), sv(sv), s(s) {}
protected:
    Io& io;
    Services& sv;
    Session& s;
    const User& me() const { return *s.user; }
    std::ostream& out() { return io.out(); }

    // Shows the menu until the user logs out, the session expires or input ends.
    void loop(const std::vector<Item>& items) {
        while (true) {
            out() << "\n" << me().menuTitle() << "  (" << me().fullName() << ")\n";
            for (std::size_t i = 0; i < items.size(); ++i) out() << "  " << i + 1 << "  " << items[i].label << "\n";
            out() << "  0  Log out\n";
            long long c = io.number("Choose: ", 0, static_cast<long long>(items.size()));
            if (c == 0) { sv.auth.logout(s); return; }
            try {
                sv.auth.checkIdle(s);
                items[static_cast<std::size_t>(c - 1)].action();
                sv.auth.touch(s);
            } catch (const SessionExpiredException& e) {
                out() << e.what() << "\n";
                return;
            } catch (const EndOfInput&) {
                throw;
            } catch (const AppException& e) {
                out() << "Error: " << e.what() << "\n";
            }
        }
    }

    void changePassword() {
        std::string a = io.secret("New password (at least 8 characters, not your username): ");
        std::string b = io.secret("Repeat the new password: ");
        if (a != b) throw ValidationException("The two passwords are not the same.");
        sv.auth.changePassword(s, a);
        out() << "Password changed.\n";
    }
    long long id(const std::string& what) { return io.number(what + " ID: ", 1, kMaxId); }

    // ----- shared list printers -----
    void printSubmissions(const std::vector<SubmissionDetail>& list) {
        out() << "ID | Course | Assignment | Student | State | Ver | Days late | Raw | Final | Stored file\n";
        for (const auto& d : list)
            out() << d.id << " | " << d.courseCode << " | " << d.assignmentTitle << " | " << d.studentName << " | "
                  << stateToText(d.state) << " | " << d.latestVersion << " | " << d.daysLate << " | "
                  << (d.hasMark ? markText(d.rawTenths) : "-") << " | " << (d.hasMark ? markText(d.finalTenths) : "-")
                  << " | " << d.latestPath << "\n";
        if (list.empty()) out() << "(none)\n";
    }
    void printAssignments(const std::vector<AssignmentView>& list) {
        out() << "ID | Course | Title | Deadline | Max | Closed | Your submission\n";
        for (const auto& v : list)
            out() << v.assignment.id << " | " << v.courseCode << " | " << v.assignment.title << " | "
                  << formatDateTime(v.effectiveDeadline) << " | " << v.assignment.maxMark << " | "
                  << (v.assignment.closed ? "yes" : "no") << " | " << (v.submissionState.empty() ? "-" : v.submissionState) << "\n";
        if (list.empty()) out() << "(none)\n";
    }
    void printCourses(const std::vector<Course>& list) {
        out() << "ID | Code | Title | Department ID\n";
        for (const auto& c : list) out() << c.id << " | " << c.code << " | " << c.title << " | " << c.departmentId << "\n";
        if (list.empty()) out() << "(none)\n";
    }

    // ----- teaching items (Lecturer and HoD) -----
    std::vector<Item> teachingItems() {
        return {
            {"Create assignment", [this] {
                printCourses(sv.assignments.coursesForLecturer(me()));
                long long course = id("Course");
                std::string title = io.required("Title: ");
                std::string text = io.ask("Instructions (optional): ");
                long long deadline = parseDateTime(io.required("Deadline (YYYY-MM-DD HH:MM): "));
                long long max = io.number("Maximum mark (1-1000): ", 1, 1000);
                out() << "Assignment created, ID " << sv.assignments.create(me(), course, title, text, deadline, max) << ".\n";
            }},
            {"List my assignments", [this] { printAssignments(sv.assignments.listForLecturer(me())); }},
            {"Grant an extension to one student", [this] {
                printAssignments(sv.assignments.listForLecturer(me()));
                long long a = id("Assignment"), student = id("Student user");
                long long when = parseDateTime(io.required("New deadline (YYYY-MM-DD HH:MM): "));
                sv.assignments.grantExtension(me(), a, student, when, io.required("Reason: "));
                out() << "Extension granted.\n";
            }},
            {"Close an assignment", [this] {
                printAssignments(sv.assignments.listForLecturer(me()));
                long long a = id("Assignment");
                if (io.yesNo("Close it? No new submissions will be accepted")) { sv.assignments.close(me(), a); out() << "Closed.\n"; }
            }},
            {"Dashboard: submissions waiting to be marked", [this] { printSubmissions(sv.marking.listUnmarked(me())); }},
            {"List submissions of an assignment", [this] {
                printAssignments(sv.assignments.listForLecturer(me()));
                printSubmissions(sv.marking.listSubmissions(me(), id("Assignment")));
            }},
            {"Mark, revise or re-mark a submission", [this] { markFlow(); }},
        };
    }
    void markFlow() {
        printAssignments(sv.assignments.listForLecturer(me()));
        long long a = id("Assignment");
        auto list = sv.marking.listSubmissions(me(), a);
        printSubmissions(list);
        long long sid = id("Submission");
        const SubmissionDetail* d = nullptr;
        for (const auto& x : list) if (x.id == sid) d = &x;
        if (!d) throw NotFoundException("That submission is not in this assignment.");
        if (d->state == State::Moderated || d->state == State::Released)
            throw BusinessRuleException("Only the Head of Department can change this mark.");
        out() << "File to read: " << d->latestPath << " (version " << d->latestVersion << ")\n";
        long long raw = parseMarkTenths(io.required("Raw mark (0-" + std::to_string(d->maxMark) + ", one decimal allowed): "), d->maxMark);
        std::string feedback = io.required("Written feedback (1-2000 characters): ");
        std::string reason;
        if (d->state != State::Submitted) reason = io.required("Reason for the change: ");
        long long fin = sv.marking.previewFinal(*d, raw);
        out() << "Days late: " << d->daysLate << "  Raw: " << markText(raw) << "  Penalty: " << markText(raw - fin)
              << "  Final: " << markText(fin) << " / " << d->maxMark << "\n";
        if (io.yesNo("Save this mark?")) {
            sv.marking.mark(me(), sid, raw, feedback, reason);
            out() << "Marked. Students cannot see it until the Head of Department releases it.\n";
        }
    }

    // ----- moderation items (HoD, or a lecturer who is acting HoD) -----
    std::vector<Item> moderationItems() {
        return {
            {"Moderation queue (marked, waiting for review)", [this] { printSubmissions(sv.moderation.queue(me())); }},
            {"Review a submission: approve, adjust or return", [this] {
                auto q = sv.moderation.queue(me());
                printSubmissions(q);
                long long sid = id("Submission");
                for (const auto& d : q) if (d.id == sid) out() << "Feedback: " << d.feedback << "\n";
                out() << "  1 Approve\n  2 Adjust the mark\n  3 Return to the lecturer\n";
                long long c = io.number("Choose: ", 1, 3);
                if (c == 1) { sv.moderation.approve(me(), sid, io.ask("Comment (optional): ")); out() << "Approved. Still hidden from the student until released.\n"; }
                if (c == 2) {
                    long long max = 0;
                    for (const auto& d : q) if (d.id == sid) max = d.maxMark;
                    long long raw = parseMarkTenths(io.required("New raw mark: "), max ? max : 1000);
                    sv.moderation.adjust(me(), sid, raw, io.required("Reason: "));
                    out() << "Adjusted and approved.\n";
                }
                if (c == 3) { sv.moderation.returnToLecturer(me(), sid, io.required("Comment for the lecturer: ")); out() << "Returned.\n"; }
            }},
            {"Release approved marks to students", [this] {
                auto ready = sv.moderation.approvedQueue(me());
                printSubmissions(ready);
                if (ready.empty()) return;
                std::string text = io.required("IDs to release, separated by commas, or 'all': ");
                std::vector<long long> chosen;
                if (trim(text) == "all") { for (const auto& d : ready) chosen.push_back(d.id); }
                else {
                    std::stringstream ss(text); std::string part;
                    while (std::getline(ss, part, ',')) {
                        try { chosen.push_back(std::stoll(trim(part))); } catch (...) { throw ValidationException("'" + part + "' is not a number."); }
                    }
                }
                if (io.yesNo("Release " + std::to_string(chosen.size()) + " submission(s) to the students")) {
                    out() << sv.moderation.release(me(), chosen) << " released.\n";
                }
            }},
            {"Amend a released mark", [this] {
                long long sid = id("Submission");
                long long raw = parseMarkTenths(io.required("Corrected raw mark: "), 1000);
                sv.moderation.amend(me(), sid, raw, io.required("Reason: "));
                out() << "Amended. An audit entry was written.\n";
            }},
        };
    }
    void showAudit() {
        for (const auto& e : sv.audit.view(me()))
            out() << "#" << e.id << " submission " << e.submissionId << " | " << formatDateTime(e.changedAt) << " | " << e.username
                  << (e.actingCapacity ? " (acting HoD)" : "") << " | " << e.action << " | raw "
                  << (e.hasOld ? markText(e.oldRaw) : "-") << " -> " << markText(e.newRaw) << " | final "
                  << (e.hasOld ? markText(e.oldFinal) : "-") << " -> " << markText(e.newFinal) << " | " << e.reason << "\n";
    }
};

class AdminMenu : public MenuBase {
public:
    using MenuBase::MenuBase;
    void run() {
        loop({
            {"Create user", [this] {
                std::string u = io.required("Username: "), n = io.required("Full name: ");
                out() << "  1 Lecturer  2 Head of Department  3 Student  4 Administrator\n";
                long long r = io.number("Role: ", 1, 4);
                Role role = r == 1 ? Role::Lecturer : r == 2 ? Role::Hod : r == 3 ? Role::Student : Role::Admin;
                std::string pw = io.required("Temporary password (at least 8 characters): ");
                out() << "User created, ID " << sv.admin.createUser(me(), u, n, role, pw) << ". The user must change the password at first login.\n";
            }},
            {"List users", [this] {
                for (const auto& u : sv.admin.listUsers(me()))
                    out() << u.id << " | " << u.username << " | " << u.fullName << " | " << roleToText(u.role) << (u.active ? "" : " | DEACTIVATED") << "\n";
            }},
            {"Deactivate a user", [this] { sv.admin.deactivate(me(), id("User")); out() << "Deactivated.\n"; }},
            {"Reset a user's password", [this] {
                long long u = id("User");
                sv.admin.resetPassword(me(), u, io.required("Temporary password: "));
                out() << "Password reset. The user must change it at next login.\n";
            }},
            {"Create department", [this] { out() << "Department created, ID " << sv.admin.createDepartment(me(), io.required("Name: ")) << ".\n"; }},
            {"Set the Head of Department", [this] {
                listDepartments();
                long long d = id("Department"), u = id("HoD user");
                sv.admin.setHead(me(), d, u); out() << "Head set.\n";
            }},
            {"Create course", [this] {
                listDepartments();
                long long d = id("Department");
                std::string code = io.required("Course code: "), title = io.required("Course title: ");
                out() << "Course created, ID " << sv.admin.createCourse(me(), d, code, title) << ".\n";
            }},
            {"Assign a lecturer to a course", [this] {
                printCourses(sv.admin.listCourses(me()));
                long long c = id("Course"), l = id("Lecturer user");
                sv.admin.assignLecturer(me(), c, l); out() << "Assigned.\n";
            }},
            {"Enrol a student in a course", [this] {
                printCourses(sv.admin.listCourses(me()));
                long long c = id("Course"), st = id("Student user");
                sv.admin.enrol(me(), c, st); out() << "Enrolled.\n";
            }},
            {"Import students from a CSV file", [this] {
                out() << "Each line: username,full name,temporary password[,course code]\n";
                out() << sv.admin.importCsv(me(), io.required("CSV file path: ")) << " students imported.\n";
            }},
            {"Designate an acting Head of Department", [this] {
                listDepartments();
                long long d = id("Department"), l = id("Lecturer user");
                sv.admin.designateActingHod(me(), d, l); out() << "Designated. Any earlier acting head was ended.\n";
            }},
            {"List departments and courses", [this] { listDepartments(); printCourses(sv.admin.listCourses(me())); }},
            {"View audit log", [this] { showAudit(); }},
            {"Back up the database", [this] {
                sv.backup.backup(me(), io.required("Backup file path (new file): "));
                out() << "Backup written.\n";
            }},
            {"Change my password", [this] { changePassword(); }},
        });
    }
private:
    void listDepartments() {
        out() << "ID | Department | Head user ID\n";
        for (const auto& d : sv.admin.listDepartments(me())) out() << d.id << " | " << d.name << " | " << d.headUserId << "\n";
    }
};

class LecturerMenu : public MenuBase {
public:
    using MenuBase::MenuBase;
    void run() {
        auto items = teachingItems();
        if (!sv.moderation.departmentsFor(me()).empty()) {         // lecturer who is currently acting HoD
            out() << "You are acting Head of Department: moderation options are added to your menu.\n";
            for (auto& i : moderationItems()) items.push_back(i);
        }
        items.push_back({"Change my password", [this] { changePassword(); }});
        loop(items);
    }
};

class HodMenu : public MenuBase {
public:
    using MenuBase::MenuBase;
    void run() {
        try {                                                       // FR12: tell the HoD what is waiting
            out() << sv.moderation.queue(me()).size() << " submission(s) are waiting for moderation.\n";
        } catch (const AppException& e) { out() << e.what() << "\n"; }
        auto items = moderationItems();
        for (auto& i : teachingItems()) items.push_back(i);
        items.push_back({"View audit log", [this] { showAudit(); }});
        items.push_back({"Change my password", [this] { changePassword(); }});
        loop(items);
    }
};

class StudentMenu : public MenuBase {
public:
    using MenuBase::MenuBase;
    void run() {
        loop({
            {"View my courses", [this] { printCourses(sv.assignments.coursesForStudent(me())); }},
            {"View my assignments", [this] { printAssignments(sv.assignments.listForStudent(me())); }},
            {"Submit or resubmit a file", [this] { submitFlow(); }},
            {"View my released marks", [this] {
                auto marks = sv.submissions.viewMarks(me());
                for (const auto& m : marks)
                    out() << m.courseCode << " - " << m.title << ": raw " << markText(m.rawTenths) << ", late days " << m.daysLate
                          << ", penalty " << markText(m.penaltyTenths) << ", final " << markText(m.finalTenths) << "/" << m.maxMark
                          << "\n  Feedback: " << m.feedback << "\n";
                if (marks.empty()) out() << "No marks have been released yet.\n";
            }},
            {"Change my password", [this] { changePassword(); }},
        });
    }
private:
    void submitFlow() {
        printAssignments(sv.assignments.listForStudent(me()));
        long long a = id("Assignment");
        std::string path = io.required("Path of the file to submit: ");
        SubmissionPlan p = sv.submissions.prepare(me(), a, path);
        if (p.isResubmission) out() << "This new file will replace version " << p.version - 1 << " for marking. Older versions are kept.\n";
        if (p.daysLate > 0) {
            out() << "WARNING: this work is " << p.daysLate << " day(s) late and a penalty applies.\n";
            if (p.daysLate >= 4) out() << "The mark will be zero.\n";
        }
        if (!io.yesNo("Confirm submission")) { out() << "Cancelled. Nothing was saved.\n"; return; }
        Receipt r = sv.submissions.confirm(me(), p);
        out() << "Receipt: submission " << r.submissionId << ", version " << r.version << ", at " << formatDateTime(r.submittedAt)
              << (r.daysLate ? ", " + std::to_string(r.daysLate) + " day(s) late" : ", on time") << ".\n";
    }
};

}  // namespace

// ================= ConsoleApp =================
void ConsoleApp::forcePasswordChange(Session& s) {
    io_.out() << "You must choose your own password before you continue.\n";
    while (true) {
        try {
            std::string a = io_.secret("New password (at least 8 characters, not your username): ");
            if (a != io_.secret("Repeat the new password: ")) throw ValidationException("The two passwords are not the same.");
            sv_.auth.changePassword(s, a);
            io_.out() << "Password changed.\n";
            return;
        } catch (const ValidationException& e) { io_.out() << e.what() << "\n"; }
    }
}
void ConsoleApp::run() {
    io_.out() << "\nModerated Learning Management System (works offline)\n";
    try {
        while (true) {
            std::string username = trim(io_.ask("\nUsername (q to quit): "));
            if (username == "q" || username == "Q") break;
            std::string password = io_.secret("Password: ");
            try {
                Session s = sv_.auth.login(username, password);
                io_.out() << "Welcome, " << s.user->fullName() << ".\n";
                if (s.user->mustChangePassword()) forcePasswordChange(s);
                sv_.auth.touch(s);
                switch (s.user->getRole()) {
                    case Role::Admin: AdminMenu(io_, sv_, s).run(); break;
                    case Role::Lecturer: LecturerMenu(io_, sv_, s).run(); break;
                    case Role::Hod: HodMenu(io_, sv_, s).run(); break;
                    default: StudentMenu(io_, sv_, s).run(); break;
                }
            } catch (const EndOfInput&) {
                throw;
            } catch (const AppException& e) {
                io_.out() << e.what() << "\n";
            }
        }
    } catch (const EndOfInput&) {
        io_.out() << "\n";
    }
    io_.out() << "Goodbye.\n";
}
