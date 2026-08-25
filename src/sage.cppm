export module sage;

// Layer 0: Vendor Bridge Modules
export import sage.vendor.lmdb;
export import sage.vendor.zstd;
export import sage.vendor.toml;
export import sage.vendor.curl;

// Layer 1: Foundation & Utilities
export import sage.util;

// Layer 2: Domain Models & Services
export import sage.package;
export import sage.config;
export import sage.build;
export import sage.service;
export import sage.channel;

// Layer 3: Storage & Archiving
export import sage.db;
export import sage.archive;

// Layer 4: Orchestration & Dependency Solving
export import sage.solver;
export import sage.repo;
export import sage.triggers;
export import sage.service_registry;
export import sage.rebuild;
