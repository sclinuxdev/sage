export module sage.util;

// Foundation utilities, partitioned by responsibility:
//   :log    ANSI colors + leveled console output
//   :str    string views, glob matching, human-readable sizes
//   :fs     path normalization, file metadata snapshots, POSIX env
//   :elf    DT_NEEDED / DT_SONAME scanner
//   :hash   SHA-256 (OpenSSL EVP, hardware accelerated)
//   :lock   process-wide operation lock
// The primary interface re-exports every partition so callers keep a single
// `import sage.util;`.

export import :log;
export import :str;
export import :fs;
export import :elf;
export import :hash;
export import :lock;
