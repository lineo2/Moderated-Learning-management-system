#include "Database.h"

namespace {

// The schema from section 6 of the System Design, with IF NOT EXISTS so it can run at every start.
// One change to the design: 'APPROVE' is added to the audit action list, because the moderation
// sequence diagram (Figure 7) records an audit entry when the HoD approves.
const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS app_user (
   user_id              INTEGER PRIMARY KEY AUTOINCREMENT,
   username             TEXT    NOT NULL UNIQUE COLLATE NOCASE,
   full_name            TEXT    NOT NULL,
   role                 TEXT    NOT NULL CHECK (role IN ('ADMIN','LECTURER','HOD','STUDENT')),
   password_hash        TEXT    NOT NULL,
   must_change_password INTEGER NOT NULL DEFAULT 1 CHECK (must_change_password IN (0,1)),
   is_active            INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0,1))
);
CREATE TABLE IF NOT EXISTS department (
   department_id INTEGER PRIMARY KEY AUTOINCREMENT,
   name          TEXT NOT NULL UNIQUE,
   head_user_id  INTEGER UNIQUE REFERENCES app_user(user_id)
);
CREATE TABLE IF NOT EXISTS course (
   course_id     INTEGER PRIMARY KEY AUTOINCREMENT,
   department_id INTEGER NOT NULL REFERENCES department(department_id),
   code          TEXT NOT NULL UNIQUE,
   title         TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS course_lecturer (
   course_id   INTEGER NOT NULL REFERENCES course(course_id) ON DELETE CASCADE,
   lecturer_id INTEGER NOT NULL REFERENCES app_user(user_id),
   PRIMARY KEY (course_id, lecturer_id)
);
CREATE TABLE IF NOT EXISTS enrolment (
   course_id   INTEGER NOT NULL REFERENCES course(course_id) ON DELETE CASCADE,
   student_id  INTEGER NOT NULL REFERENCES app_user(user_id),
   enrolled_on INTEGER NOT NULL,
   PRIMARY KEY (course_id, student_id)
);
CREATE TABLE IF NOT EXISTS acting_hod (
   acting_id     INTEGER PRIMARY KEY AUTOINCREMENT,
   department_id INTEGER NOT NULL REFERENCES department(department_id),
   lecturer_id   INTEGER NOT NULL REFERENCES app_user(user_id),
   start_at      INTEGER NOT NULL,
   end_at        INTEGER,
   CHECK (end_at IS NULL OR end_at > start_at)
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_acting_one_active ON acting_hod(department_id) WHERE end_at IS NULL;
CREATE TABLE IF NOT EXISTS assignment (
   assignment_id INTEGER PRIMARY KEY AUTOINCREMENT,
   course_id     INTEGER NOT NULL REFERENCES course(course_id),
   title         TEXT NOT NULL,
   description   TEXT NOT NULL DEFAULT '',
   deadline      INTEGER NOT NULL,
   max_mark      INTEGER NOT NULL CHECK (max_mark BETWEEN 1 AND 1000),
   is_closed     INTEGER NOT NULL DEFAULT 0 CHECK (is_closed IN (0,1)),
   created_by    INTEGER NOT NULL REFERENCES app_user(user_id),
   UNIQUE (course_id, title)
);
CREATE TABLE IF NOT EXISTS extension (
   extension_id  INTEGER PRIMARY KEY AUTOINCREMENT,
   assignment_id INTEGER NOT NULL REFERENCES assignment(assignment_id),
   student_id    INTEGER NOT NULL REFERENCES app_user(user_id),
   granted_by    INTEGER NOT NULL REFERENCES app_user(user_id),
   old_deadline  INTEGER NOT NULL,
   new_deadline  INTEGER NOT NULL,
   reason        TEXT NOT NULL CHECK (length(trim(reason)) > 0),
   granted_at    INTEGER NOT NULL,
   CHECK (new_deadline > old_deadline)
);
CREATE TABLE IF NOT EXISTS submission (
   submission_id     INTEGER PRIMARY KEY AUTOINCREMENT,
   assignment_id     INTEGER NOT NULL REFERENCES assignment(assignment_id),
   student_id        INTEGER NOT NULL REFERENCES app_user(user_id),
   state             TEXT NOT NULL DEFAULT 'SUBMITTED'
                     CHECK (state IN ('SUBMITTED','MARKED','RETURNED','MODERATED','RELEASED')),
   raw_tenths        INTEGER CHECK (raw_tenths IS NULL OR raw_tenths >= 0),
   feedback          TEXT NOT NULL DEFAULT '' CHECK (length(feedback) <= 2000),
   marked_by         INTEGER REFERENCES app_user(user_id),
   moderated_by      INTEGER REFERENCES app_user(user_id),
   moderator_comment TEXT NOT NULL DEFAULT '',
   released_at       INTEGER,
   UNIQUE (assignment_id, student_id),
   CHECK (state = 'SUBMITTED' OR (raw_tenths IS NOT NULL AND marked_by IS NOT NULL)),
   CHECK (state NOT IN ('MODERATED','RELEASED') OR moderated_by IS NOT NULL),
   CHECK ((state = 'RELEASED') = (released_at IS NOT NULL))
);
CREATE TABLE IF NOT EXISTS submission_version (
   version_id    INTEGER PRIMARY KEY AUTOINCREMENT,
   submission_id INTEGER NOT NULL REFERENCES submission(submission_id) ON DELETE CASCADE,
   version_no    INTEGER NOT NULL CHECK (version_no BETWEEN 1 AND 5),
   original_name TEXT NOT NULL,
   stored_path   TEXT NOT NULL,
   file_size     INTEGER NOT NULL CHECK (file_size >= 1),
   submitted_at  INTEGER NOT NULL,
   UNIQUE (submission_id, version_no)
);
CREATE TABLE IF NOT EXISTS audit_entry (
   audit_id         INTEGER PRIMARY KEY AUTOINCREMENT,
   submission_id    INTEGER NOT NULL REFERENCES submission(submission_id),
   changed_by       INTEGER NOT NULL REFERENCES app_user(user_id),
   changed_at       INTEGER NOT NULL,
   action           TEXT NOT NULL CHECK (action IN ('MARK','REVISE','APPROVE','ADJUST','RETURN','RELEASE','AMEND')),
   old_raw_tenths   INTEGER,
   new_raw_tenths   INTEGER NOT NULL,
   old_final_tenths INTEGER,
   new_final_tenths INTEGER NOT NULL,
   reason           TEXT NOT NULL CHECK (length(trim(reason)) > 0),
   acting_capacity  INTEGER NOT NULL DEFAULT 0 CHECK (acting_capacity IN (0,1))
);
CREATE INDEX IF NOT EXISTS ix_course_dept     ON course(department_id);
CREATE INDEX IF NOT EXISTS ix_cl_lecturer     ON course_lecturer(lecturer_id);
CREATE INDEX IF NOT EXISTS ix_enrol_student   ON enrolment(student_id);
CREATE INDEX IF NOT EXISTS ix_acting_lecturer ON acting_hod(lecturer_id);
CREATE INDEX IF NOT EXISTS ix_assign_course   ON assignment(course_id);
CREATE INDEX IF NOT EXISTS ix_ext_lookup      ON extension(assignment_id, student_id);
CREATE INDEX IF NOT EXISTS ix_sub_student     ON submission(student_id);
CREATE INDEX IF NOT EXISTS ix_sub_state       ON submission(state);
CREATE INDEX IF NOT EXISTS ix_audit_sub       ON audit_entry(submission_id, changed_at);

CREATE TRIGGER IF NOT EXISTS tg_dept_head_role BEFORE INSERT ON department
WHEN NEW.head_user_id IS NOT NULL AND (SELECT role FROM app_user WHERE user_id = NEW.head_user_id) <> 'HOD'
BEGIN SELECT RAISE(ABORT, 'head_user_id must be a HOD'); END;
CREATE TRIGGER IF NOT EXISTS tg_dept_head_role_u BEFORE UPDATE OF head_user_id ON department
WHEN NEW.head_user_id IS NOT NULL AND (SELECT role FROM app_user WHERE user_id = NEW.head_user_id) <> 'HOD'
BEGIN SELECT RAISE(ABORT, 'head_user_id must be a HOD'); END;
CREATE TRIGGER IF NOT EXISTS tg_cl_role BEFORE INSERT ON course_lecturer
WHEN (SELECT role FROM app_user WHERE user_id = NEW.lecturer_id) NOT IN ('LECTURER','HOD')
BEGIN SELECT RAISE(ABORT, 'only lecturers may teach a course'); END;
CREATE TRIGGER IF NOT EXISTS tg_enrol_role BEFORE INSERT ON enrolment
WHEN (SELECT role FROM app_user WHERE user_id = NEW.student_id) <> 'STUDENT'
BEGIN SELECT RAISE(ABORT, 'only students may be enrolled'); END;
CREATE TRIGGER IF NOT EXISTS tg_acting_role BEFORE INSERT ON acting_hod
WHEN (SELECT role FROM app_user WHERE user_id = NEW.lecturer_id) <> 'LECTURER'
BEGIN SELECT RAISE(ABORT, 'acting HoD must be a lecturer'); END;
CREATE TRIGGER IF NOT EXISTS tg_assign_immutable BEFORE UPDATE OF max_mark, deadline, course_id ON assignment
WHEN NEW.max_mark <> OLD.max_mark OR NEW.deadline <> OLD.deadline OR NEW.course_id <> OLD.course_id
BEGIN SELECT RAISE(ABORT, 'max_mark, deadline and course are fixed'); END;
CREATE TRIGGER IF NOT EXISTS tg_sub_mark_ins BEFORE INSERT ON submission
WHEN NEW.raw_tenths IS NOT NULL AND NEW.raw_tenths > 10 * (SELECT max_mark FROM assignment WHERE assignment_id = NEW.assignment_id)
BEGIN SELECT RAISE(ABORT, 'mark exceeds max_mark'); END;
CREATE TRIGGER IF NOT EXISTS tg_sub_mark_upd BEFORE UPDATE OF raw_tenths ON submission
WHEN NEW.raw_tenths IS NOT NULL AND NEW.raw_tenths > 10 * (SELECT max_mark FROM assignment WHERE assignment_id = NEW.assignment_id)
BEGIN SELECT RAISE(ABORT, 'mark exceeds max_mark'); END;
CREATE TRIGGER IF NOT EXISTS tg_sub_independence BEFORE UPDATE OF moderated_by ON submission
WHEN NEW.moderated_by IS NOT NULL AND EXISTS (
  SELECT 1 FROM course_lecturer cl JOIN assignment a ON a.course_id = cl.course_id
  WHERE a.assignment_id = NEW.assignment_id AND cl.lecturer_id = NEW.moderated_by)
BEGIN SELECT RAISE(ABORT, 'moderator teaches this course'); END;
CREATE TRIGGER IF NOT EXISTS tg_audit_no_update BEFORE UPDATE ON audit_entry
BEGIN SELECT RAISE(ABORT, 'audit_entry is append-only'); END;
CREATE TRIGGER IF NOT EXISTS tg_audit_no_delete BEFORE DELETE ON audit_entry
BEGIN SELECT RAISE(ABORT, 'audit_entry is append-only'); END;
)SQL";

// RAII wrapper so a prepared statement is always finalized.
class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw DatabaseException(sqlite3_errmsg(db_));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(const std::vector<Value>& params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            int idx = static_cast<int>(i) + 1, rc;
            if (std::holds_alternative<long long>(params[i]))
                rc = sqlite3_bind_int64(stmt_, idx, std::get<long long>(params[i]));
            else if (std::holds_alternative<std::string>(params[i]))
                rc = sqlite3_bind_text(stmt_, idx, std::get<std::string>(params[i]).c_str(), -1, SQLITE_TRANSIENT);
            else
                rc = sqlite3_bind_null(stmt_, idx);
            if (rc != SQLITE_OK) throw DatabaseException(sqlite3_errmsg(db_));
        }
    }
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DatabaseException(sqlite3_errmsg(db_));
    }
    Row row() const {
        Row r;
        int n = sqlite3_column_count(stmt_);
        for (int i = 0; i < n; ++i) {
            switch (sqlite3_column_type(stmt_, i)) {
                case SQLITE_NULL: r.cells.emplace_back(std::monostate{}); break;
                case SQLITE_INTEGER: r.cells.emplace_back(static_cast<long long>(sqlite3_column_int64(stmt_, i))); break;
                default: r.cells.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt_, i))));
            }
        }
        return r;
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "could not open the database";
        if (db_) sqlite3_close(db_);
        throw DatabaseException(msg);
    }
    sqlite3_busy_timeout(db_, 3000);
    execute("PRAGMA foreign_keys = ON");
    char* err = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "schema error";
        sqlite3_free(err);
        sqlite3_close(db_);
        throw DatabaseException(msg);
    }
}
Database::~Database() { if (db_) sqlite3_close(db_); }

std::vector<Row> Database::query(const std::string& sql, const std::vector<Value>& params) {
    Statement s(db_, sql);
    s.bind(params);
    std::vector<Row> rows;
    while (s.step()) rows.push_back(s.row());
    return rows;
}
long long Database::execute(const std::string& sql, const std::vector<Value>& params) {
    Statement s(db_, sql);
    s.bind(params);
    while (s.step()) {}
    return sqlite3_changes(db_);
}
void Database::backupTo(const std::string& path) {
    sqlite3* dest = nullptr;
    if (sqlite3_open(path.c_str(), &dest) != SQLITE_OK) {
        std::string msg = dest ? sqlite3_errmsg(dest) : "could not create the backup file";
        if (dest) sqlite3_close(dest);
        throw DatabaseException(msg);
    }
    sqlite3_backup* b = sqlite3_backup_init(dest, "main", db_, "main");
    if (!b) { std::string msg = sqlite3_errmsg(dest); sqlite3_close(dest); throw DatabaseException(msg); }
    sqlite3_backup_step(b, -1);
    int rc = sqlite3_backup_finish(b);
    std::string msg = sqlite3_errmsg(dest);
    sqlite3_close(dest);
    if (rc != SQLITE_OK) throw DatabaseException("Backup failed: " + msg);
}
