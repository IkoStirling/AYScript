#pragma once
#include <AYEntity/DeterministicSession.h>
namespace ayt::script {
struct DetLogiaOptions {
    std::uint32_t maxOperations=10000;
    std::uint32_t maxCallDepth=16;
};
class DeterministicLogiaProgram;
/// Parse and type-check the restricted System profile before registering any callback.
/// Uses Logia syntax, software float/vec3, checked int64/bool and registered Sim APIs.
/// Rejects shared/persistent script variables, imports, async, native host providers and IO.
std::shared_ptr<const DeterministicLogiaProgram> compileDeterministicLogia(
    const std::string& source,DetLogiaOptions options,std::string& error);
/** @brief Immutable deterministic Logia System program, independent of Lua and native providers.
 * @note Link AYScript::Determinism. Exactly one on_update(dt:float) entry; float
 * means software binary32. Locals exist only during a tick; persistent state uses
 * the typed Sim read/write facade. Every evaluation is budgeted; failure faults
 * the Session until explicit restore.
 */
class DeterministicLogiaProgram {
public:
    static constexpr std::uint32_t kProfileVersion=1;
    std::uint64_t hash() const;
    /// Prefer installDeterministicLogia so code/options identity enters the manifest.
    bool execute(entity::DetTickContext&) const;
private:
    struct Impl;
    explicit DeterministicLogiaProgram(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> _impl;
    friend std::shared_ptr<const DeterministicLogiaProgram> compileDeterministicLogia(
        const std::string&,DetLogiaOptions,std::string&);
};
/// Register immutable code and its identity before seal; peers/replays compare manifest4.
/// A failed installation requires abandoning this configuration, like multi-step installers.
bool installDeterministicLogia(entity::DeterministicSession&,std::uint32_t systemId,
    std::int32_t priority,std::shared_ptr<const DeterministicLogiaProgram>);
}
