#include "Sora/Core/CLI.h"
#include "Sora/Core/CLI/Descriptions.h"

using namespace Sora;
using namespace Sora::CLI;

struct Commit {};
struct Push {};

struct MyProgram {
    static consteval void BuildSchema(SchemaBuilder<MyProgram>& builder) noexcept {
        builder.Name("greet").Policy(Policy::Utf8 | Policy::GnuStyle).Command<Commit>().Command<Push>();
    }
};

inline constexpr auto program = Compile<MyProgram>();

int main() {
    return 0;
}
