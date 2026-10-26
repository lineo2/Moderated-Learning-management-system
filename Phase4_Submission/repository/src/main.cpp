// Composition root: the concrete classes are created once here and passed in through constructors.
#include <filesystem>
#include <iostream>

#include "Console.h"

int main(int argc, char* argv[]) {
    std::string arg = argc > 1 ? argv[1] : "";
    if (arg == "--help" || arg == "-h") {
        std::cout << "Moderated Learning Management System\n"
                     "Usage: mlms [database-file]      (default: mlms.db in the current folder)\n"
                     "Submitted files are stored in a 'submissions' folder next to the database file.\n";
        return 0;
    }
    try {
        std::filesystem::path dbPath = arg.empty() ? "mlms.db" : arg;
        std::filesystem::path folder = dbPath.has_parent_path() ? dbPath.parent_path() : std::filesystem::path(".");

        SystemClock clock;
        Argon2Hasher hasher;
        LocalFileStore files(folder / "submissions");
        TenPercentPerDayPolicy policy;
        Database db(dbPath.string());

        UserRepository users(db);
        AcademicRepository academic(db);
        AssignmentRepository assignmentRepo(db);
        SubmissionRepository submissionRepo(db);
        AuditRepository auditRepo(db);

        AuditService audit(auditRepo);
        AuthService auth(users, hasher, clock);
        AdminService admin(db, users, academic, hasher, clock);
        AssignmentService assignments(assignmentRepo, academic, clock);
        SubmissionService submissions(db, submissionRepo, assignmentRepo, academic, files, clock, policy);
        MarkingService marking(db, submissionRepo, assignmentRepo, academic, audit, policy, clock);
        ModerationService moderation(db, submissionRepo, academic, audit, policy, clock);
        BackupService backup(db);

        if (admin.createFirstAdminIfNeeded("ChangeMe123!"))
            std::cout << "\nFirst run: administrator account created.\n"
                         "  username: admin    temporary password: ChangeMe123!\n"
                         "You will be asked to choose a new password at the first login.\n";

        Services sv{auth, admin, assignments, submissions, marking, moderation, audit, backup};
        Io io(std::cin, std::cout);
        ConsoleApp(io, sv).run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
