 Moderated Learning Management System 

A C++17 console application for a secondary school to manage courses, assignments, submissions, marking, Head-of-Department moderation, and secure release of marks. The system runs entirely offline, stores data in a local SQLite database, and enforces strict role-based access control.

1. Build and Run Guide

Prerequisites:
- C++17 compatible compiler (GCC, Clang, or MSVC)
- CMake (version 3.16 or higher)
- SQLite3 (development headers)
- libsodium (development headers for Argon2id hashing)

Linux / macOS
Run the following commands from the root directory:
```bash
Install dependencies (Ubuntu/Debian example)
sudo apt update
sudo apt install build-essential cmake libsqlite3-dev libsodium-dev pkg-config

2.Configure, build, and test
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

Run the application
./build/mlms