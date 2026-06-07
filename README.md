# sop-backup — Real-Time File Backup Daemon

A command-line backup tool for Linux that monitors source directories and mirrors changes to backup targets in real time using `inotify`. Written in C as part of the "Operating Systems 1" course at Warsaw University of Technology.

## How it works

The program runs as an interactive shell. You add backup pairs (source → target), and it spawns a child process for each pair that:
1. Performs a full initial copy of the source directory
2. Watches for file system events (create, modify, delete, move) via `inotify`
3. Mirrors every change to the backup target in real time

## Commands

```
add "/path/to/source" "/path/to/backup1" "/path/to/backup2"   — start backing up
end "/path/to/source" "/path/to/backup1"                      — stop a backup pair
list                                                           — show active backups
restore "/path/to/source" "/path/to/backup"                   — restore source from backup
exit                                                           — stop all backups and quit
```

## Key implementation details

- Each backup pair runs as a separate child process (`fork`)
- File system monitoring via Linux `inotify` API with recursive watch setup
- Atomic initial copy using a temp directory + `rename`
- Signal handling: graceful shutdown on SIGTERM/SIGINT, ignores irrelevant signals
- Symlink-aware copying with target path adjustment
- Optimized restore: only overwrites files that changed since backup creation
- Move operations mirrored via `cookie`-based MOVED_FROM/MOVED_TO pairing

## Build & run

```bash
make
./sop-backup
```

Requires Linux (uses `inotify`, `fork`, POSIX signals).

## Tech

C17, POSIX API, inotify, fork/waitpid, signals
