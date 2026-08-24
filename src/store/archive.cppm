export module sage.archive;

// Streaming package archive engine, partitioned by responsibility:
//   :core     shared constants + inspection/extraction result structs
//   :idx      .METADATA/files.idx per-file integrity index I/O
//   :detail   anchored path safety (never escape the target root) + removal
//   :inspect  leading-.METADATA reader + constant-cost package inspection
//   :extract  one decompression pass, parallel anchored writes
//   :pack     reproducible package creation + repository index generation
// The primary interface re-exports every partition so callers keep a single
// `import sage.archive;`.

export import :core;
export import :idx;
export import :detail;
export import :inspect;
export import :extract;
export import :pack;
