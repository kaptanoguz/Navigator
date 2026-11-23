// Internal (src/platform): test seam of the default IProcessRunner. Production code uses
// platform::create_default_process_runner(), which is create_process_runner({}).
#pragma once

#include <memory>
#include <optional>

#include <navigator/platform.hpp>
#include <navigator/types.hpp>

namespace navigator::platform_internal {

struct ProcessRunnerOptions {
    // How `bwrap` is found for ProcessRequest::sandbox:
    //   Lookup: find_program("bwrap") on each run (production).
    //   Absent: behave as if bwrap were not installed.
    //   Fixed:  use `bwrap_path` (tests point it at a fake bwrap script).
    enum class Bwrap { Lookup, Absent, Fixed };
    Bwrap bwrap = Bwrap::Lookup;
    Path bwrap_path;
};

// The default runner with injectable bwrap discovery.
//
// Sandbox fallback: before the first sandboxed run with a given bwrap binary the runner probes
// it once (bwrap with the same namespace flags and system binds running `true`, 5 s limit).
// If the probe fails (for example unprivileged user namespaces are disabled), sandboxed runs
// use the unsandboxed path and report ProcessResult::sandboxed = false. The probe result is
// cached in the runner instance (mutex-protected). A real run that fails inside bwrap after a
// successful probe is a non-zero exit and is never retried unsandboxed.
[[nodiscard]] Result<std::unique_ptr<platform::IProcessRunner>>
create_process_runner(ProcessRunnerOptions options);

} // namespace navigator::platform_internal
