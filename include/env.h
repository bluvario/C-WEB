#ifndef CWEB_ENV_H
#define CWEB_ENV_H

// a tiny dotenv loader for generated servers (and anything else that wants
// flags to be overridable by a checked-in file). it exists so an app's
// environment — PORT, a database path, feature switches — can live in a file
// the deploy runs with, keeping the flag line short and the same binary
// deployable without editing it.
//
// semantics deliberately conservative:
//   * blank lines and '#' comments are ignored;
//   * an optional leading "export " is tolerated so a shell-style file loads
//     as-is;
//   * KEY=VALUE: the key must be a plain identifier; the value is trimmed and
//     surrounding single or double quotes are stripped (no escapes inside);
//   * existing variables in the real environment win: setenv is never allowed
//     to overwrite, so a shell that already exported PORT keeps its value.

// loads path's KEY=VALUE pairs into the process environment. returns 0 on
// success, -1 when the file cannot be opened. lines that do not match the
// grammar are skipped, not fatal.
int cweb_env_file(const char *path);

#endif