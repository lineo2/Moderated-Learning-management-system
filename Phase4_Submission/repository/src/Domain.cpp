#include "Domain.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

// ---------- Roles, permissions, users ----------
std::string roleToText(Role r) {
    switch (r) {
        case Role::Admin: return "ADMIN";
        case Role::Lecturer: return "LECTURER";
        case Role::Hod: return "HOD";
        default: return "STUDENT";
    }
}
Role roleFromText(const std::string& s) {
    if (s == "ADMIN") return Role::Admin;
    if (s == "LECTURER") return Role::Lecturer;
    if (s == "HOD") return Role::Hod;
    if (s == "STUDENT") return Role::Student;
    throw ValidationException("Unknown role: " + s);
}

std::set<Permission> Administrator::permissions() const {
    return {Permission::ManageUsers, Permission::ManageCourses, Permission::ViewAudit, Permission::Backup};
}
std::set<Permission> Lecturer::permissions() const {
    return {Permission::CreateAssignment, Permission::GrantExtension, Permission::MarkSubmission};
}
std::set<Permission> HeadOfDepartment::permissions() const {
    return {Permission::CreateAssignment, Permission::GrantExtension, Permission::MarkSubmission,
            Permission::Moderate, Permission::ViewAudit};
}
std::set<Permission> Student::permissions() const {
    return {Permission::SubmitWork, Permission::ViewOwnMarks};
}
std::unique_ptr<User> UserFactory::create(const UserRecord& r) {
    switch (r.role) {
        case Role::Admin: return std::make_unique<Administrator>(r);
        case Role::Lecturer: return std::make_unique<Lecturer>(r);
        case Role::Hod: return std::make_unique<HeadOfDepartment>(r);
        default: return std::make_unique<Student>(r);
    }
}
void requirePermission(const User& u, Permission p) {
    if (!u.can(p)) throw AuthorizationException("Access denied: your role may not do this.");
}

// ---------- Submission states ----------
std::string stateToText(State s) {
    switch (s) {
        case State::Submitted: return "SUBMITTED";
        case State::Marked: return "MARKED";
        case State::Returned: return "RETURNED";
        case State::Moderated: return "MODERATED";
        default: return "RELEASED";
    }
}
State stateFromText(const std::string& s) {
    for (State st : {State::Submitted, State::Marked, State::Returned, State::Moderated, State::Released})
        if (stateToText(st) == s) return st;
    throw DatabaseException("Unknown submission state: " + s);
}

void SubmissionState::illegal(const char* what) const {
    throw IllegalTransitionException(std::string("Not allowed: cannot ") + what +
                                     " a submission that is " + stateToText(id()) + ".");
}
State SubmissionState::resubmit() const { illegal("resubmit"); }
State SubmissionState::mark() const { illegal("mark"); }
State SubmissionState::revise() const { illegal("revise"); }
State SubmissionState::approve() const { illegal("approve"); }
State SubmissionState::adjust() const { illegal("adjust"); }
State SubmissionState::returnToLecturer() const { illegal("return"); }
State SubmissionState::release() const { illegal("release"); }
State SubmissionState::amend() const { illegal("amend"); }

namespace {
struct SubmittedState : SubmissionState {
    State id() const override { return State::Submitted; }
    State resubmit() const override { return State::Submitted; }
    State mark() const override { return State::Marked; }
};
struct MarkedState : SubmissionState {
    State id() const override { return State::Marked; }
    State revise() const override { return State::Marked; }
    State approve() const override { return State::Moderated; }
    State adjust() const override { return State::Moderated; }
    State returnToLecturer() const override { return State::Returned; }
};
struct ReturnedState : SubmissionState {
    State id() const override { return State::Returned; }
    State mark() const override { return State::Marked; }       // lecturer re-marks
};
struct ModeratedState : SubmissionState {
    State id() const override { return State::Moderated; }
    State release() const override { return State::Released; }
};
struct ReleasedState : SubmissionState {
    State id() const override { return State::Released; }
    State amend() const override { return State::Released; }
};
}  // namespace

const SubmissionState& SubmissionState::of(State s) {
    static const SubmittedState submitted;
    static const MarkedState marked;
    static const ReturnedState returned;
    static const ModeratedState moderated;
    static const ReleasedState released;
    switch (s) {
        case State::Submitted: return submitted;
        case State::Marked: return marked;
        case State::Returned: return returned;
        case State::Moderated: return moderated;
        default: return released;
    }
}

// ---------- Penalty ----------
long long TenPercentPerDayPolicy::finalTenths(long long raw, long long maxMark, int daysLate) const {
    if (daysLate <= 0) return raw;
    if (daysLate >= 4) return 0;
    // 10% of the maximum mark, in tenths of a mark, is exactly maxMark (e.g. max 100 -> 10.0 marks = 100 tenths).
    return std::max(0LL, raw - maxMark * daysLate);
}
int DaysLateCalculator::daysLate(long long submittedAt, long long effectiveDeadline) {
    if (submittedAt <= effectiveDeadline) return 0;
    long long late = submittedAt - effectiveDeadline;
    return static_cast<int>((late + 86399) / 86400);
}
void fillDerived(SubmissionDetail& d, const PenaltyPolicy& policy) {
    d.daysLate = DaysLateCalculator::daysLate(d.latestSubmittedAt, d.effectiveDeadline);
    d.finalTenths = d.hasMark ? policy.finalTenths(d.rawTenths, d.maxMark, d.daysLate) : 0;
}

// ---------- Helpers ----------
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
long long parseMarkTenths(const std::string& input, long long maxMark) {
    std::string t = trim(input);
    std::size_t dot = t.find('.');
    std::string whole = dot == std::string::npos ? t : t.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : t.substr(dot + 1);
    auto digits = [](const std::string& x) {
        return std::all_of(x.begin(), x.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    if (whole.empty() || whole.size() > 6 || !digits(whole) || frac.size() > 1 || !digits(frac))
        throw ValidationException("The mark must be a number with at most one decimal place, e.g. 40 or 40.5.");
    long long tenths = std::stoll(whole) * 10 + (frac.empty() ? 0 : frac[0] - '0');
    if (tenths > maxMark * 10)
        throw ValidationException("The mark must be between 0 and " + std::to_string(maxMark) + ".");
    return tenths;
}
std::string markText(long long tenths) {
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}
void requireText(const std::string& value, const std::string& field, std::size_t maxLen) {
    if (trim(value).empty() || value.size() > maxLen)
        throw ValidationException(field + " must be between 1 and " + std::to_string(maxLen) + " characters.");
}
void checkPasswordRule(const std::string& password, const std::string& username) {
    if (password.size() < 8) throw ValidationException("The password must have at least 8 characters.");
    if (password == username) throw ValidationException("The password must not be the same as the username.");
}
long long parseDateTime(const std::string& text) {
    int y, mo, d, h, mi;
    char extra;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d%c", &y, &mo, &d, &h, &mi, &extra) != 5)
        throw ValidationException("Use the date format YYYY-MM-DD HH:MM, e.g. 2026-11-05 17:00.");
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi;
    tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1) || tm.tm_year != y - 1900 || tm.tm_mon != mo - 1 ||
        tm.tm_mday != d || tm.tm_hour != h)
        throw ValidationException("That date or time does not exist.");
    return static_cast<long long>(t);
}
std::string formatDateTime(long long seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}
