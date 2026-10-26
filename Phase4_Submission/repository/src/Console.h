// PRESENTATION LAYER: console menus. Reads input, checks its format, shows results and errors.
// No business rules here: every rule lives in the services.
#pragma once
#include <iostream>
#include <string>

#include "Services.h"

struct EndOfInput : std::runtime_error {       // input stream closed: leave the program cleanly
    EndOfInput() : std::runtime_error("end of input") {}
};

class Io {
public:
    Io(std::istream& in, std::ostream& out) : in_(in), out_(out) {}
    std::ostream& out() { return out_; }
    std::string ask(const std::string& prompt);                        // one line, may be empty
    std::string required(const std::string& prompt);                   // repeats until not blank
    long long number(const std::string& prompt, long long lo, long long hi);   // repeats until valid
    bool yesNo(const std::string& prompt);
    std::string secret(const std::string& prompt);                     // password: hidden when on a real terminal
private:
    std::istream& in_;
    std::ostream& out_;
};

struct Services {                              // everything the menus call
    AuthService& auth;
    AdminService& admin;
    AssignmentService& assignments;
    SubmissionService& submissions;
    MarkingService& marking;
    ModerationService& moderation;
    AuditService& audit;
    BackupService& backup;
};

class ConsoleApp {
public:
    ConsoleApp(Io& io, Services& sv) : io_(io), sv_(sv) {}
    void run();                                // login loop; returns when the user quits or input ends
private:
    void forcePasswordChange(Session& s);
    Io& io_;
    Services& sv_;
};
