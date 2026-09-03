#include <co2/config.hpp>
#include <co2/sync_wait.hpp>
#include <co2/task.hpp>

auto answer() CO2_BEG(co2::Task<int>, ()) { CO2_RETURN(42); }
CO2_END

int main() {
    static_assert(CO2_VERSION_MAJOR == 0, "unexpected co2 major version");
    static_assert(CO2_VERSION_MINOR == 2, "unexpected co2 minor version");
    static_assert(CO2_VERSION_PATCH == 0, "unexpected co2 patch version");
    return co2::syncWait(answer()) == 42 ? 0 : 1;
}
