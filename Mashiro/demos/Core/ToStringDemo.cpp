#include <print>

#include "Mashiro/Core/ToString.h"
#include "Mashiro/Platform/SystemEvent.h"

struct {
    float x{1.0f};
    float y{2.0f};
} anomy;

enum class ClassEnum {
    ClassEnumValue1,
    ClassEnumValue2,
};

enum SomeEnum { Value1, Value2, Value3 };

enum {
    Anomy1 = 0x01,
    Anomy2 = 0x02,
};

struct Named {
    SomeEnum enu = Value1;
    ClassEnum cls = ClassEnum::ClassEnumValue1;
    float x = 1.0f;
    int y = 2;
};

using namespace Mashiro;

int main() {
    std::println("{}", ToString(Named{}));
    std::println("{}", ToString(anomy));
    std::println("{}", ToString(Value1));
    std::println("{}", ToString(static_cast<decltype(Anomy1)>(Anomy1 | Anomy2)));
    std::println("{}", ToString(ClassEnum::ClassEnumValue1));
    std::println("{}", Traits::TypeName<SystemEvent>);
    return 0;
}
