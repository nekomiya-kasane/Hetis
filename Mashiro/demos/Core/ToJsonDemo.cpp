// SPDX-License-Identifier: MIT
//
// Walking tour of Mashiro::ToJson / Mashiro::FromJson.
//
// Builds and runs as a console executable: every section prints a small,
// labelled snippet so you can see exactly what each annotation does to the
// produced JSON. Sections are independent — comment any out to focus.

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <variant>
#include <vector>

#include "Mashiro/Core/Flags.h"
#include "Mashiro/Core/ToJson.h"

using namespace Mashiro;
namespace An = Json::Anno;

// =============================================================================
// 1.  Plain reflectable aggregates
// =============================================================================

namespace Demo1 {

    enum class Color { Red, Green, Blue };

    struct Person {
        std::string              name;
        int                      age = 0;
        Color                    favourite = Color::Red;
        std::vector<std::string> hobbies;
    };

    void Run() {
        Person p{"Alice", 30, Color::Green, {"reading", "hiking"}};

        // ToJson(...) walks every public NSDM via P2996 reflection.
        // Enum values come out as their identifier string by default.
        json j = ToJson(p);
        std::println("[1] plain aggregate:\n{}\n", j.dump(2));

        // Round-trip back to a Person.
        Person back = FromJson<Person>(j);
        std::println("[1] round-trip name = {}, age = {}\n", back.name, back.age);
    }

} // namespace Demo1

// =============================================================================
// 2.  Annotations: Ignore / Rename<""> / Order / Required / Optional
// =============================================================================

namespace Demo2 {

    struct Account {
        // "kept" + "displayName" emit; "transientCache" never does;
        // "userId" must be present in the input or FromJson throws;
        // "nickname" is happy to be missing — it stays at its default.
        int                                          kept = 1;
        [[= An::Ignore{}]] int                       transientCache = 9999;
        [[= An::Rename<"displayName">{}]] std::string label;
        [[= An::Required{}]] uint64_t                userId = 0;
        [[= An::Optional{}]] std::string             nickname = "<none>";
        [[= An::Order{0}]] int                       firstField = 0;
        [[= An::Order{1}]] int                       secondField = 0;
    };

    void Run() {
        Account a{};
        a.kept       = 5;
        a.label      = "Alice";
        a.userId     = 0xDEADBEEF;
        a.firstField = 100;
        a.secondField = 200;

        json j = ToJson(a);
        std::println("[2] annotated struct:\n{}\n", j.dump(2));
        std::println("[2] note: 'transientCache' is absent, 'label' is renamed,\n"
                     "        'firstField' emits before 'secondField' regardless of\n"
                     "        declaration order.\n");

        // Drop the optional key, keep the required one — should still parse.
        j.erase("nickname");
        Account back = FromJson<Account>(j);
        std::println("[2] missing optional → kept default '{}'\n\n", back.nickname);
    }

} // namespace Demo2

// =============================================================================
// 3.  Flatten — splice a nested object's members into the parent
// =============================================================================

namespace Demo3 {

    struct EventHeader {
        uint32_t sequence = 0;
        uint64_t timestamp = 0;
    };

    struct LogEvent {
        [[= An::Flatten{}]] EventHeader header; // splices into parent object
        std::string                     body;
    };

    void Run() {
        LogEvent e{{42, 1'700'000'000ULL}, "user signed in"};
        json     j = ToJson(e);
        std::println("[3] flattened nested object:\n{}\n", j.dump(2));
        std::println("[3] note: there is no \"header\" key — its fields are\n"
                     "        spliced directly into the outer object.\n\n");
    }

} // namespace Demo3

// =============================================================================
// 4.  Bitfield enums + AsInt encoding
// =============================================================================

namespace Demo4 {

    enum class Permissions : uint8_t {
        None  = 0,
        Read  = 1u << 0,
        Write = 1u << 1,
        Exec  = 1u << 2,
    };

    enum class Severity { Trace, Debug, Info, Warn, Error };

    struct Record {
        Permissions             rights = Permissions::Read | Permissions::Write;
        [[= An::AsInt{}]] Severity level  = Severity::Warn;
    };

    void Run() {
        Record r{};
        json   j = ToJson(r);
        std::println("[4] bitfield + AsInt enum:\n{}\n", j.dump(2));
        std::println("[4] note: 'rights' is a bitfield → \"Read|Write\";\n"
                     "        'level' is annotated AsInt → emits as 3.\n\n");

        Record back = FromJson<Record>(j);
        std::println("[4] round-trip preserves both encodings: rights bits = {}\n\n",
                     static_cast<int>(back.rights));
    }

} // namespace Demo4

// =============================================================================
// 5.  optional / variant / tuple / chrono / filesystem / map
// =============================================================================

namespace Demo5 {

    struct Bundle {
        std::optional<int>                          counter;
        std::optional<std::string>                  comment;
        std::variant<int, std::string, double>      payload;
        std::pair<std::string, int>                 tagged{"score", 42};
        std::map<std::string, int>                  scores{{"alice", 1}, {"bob", 2}};
        std::chrono::milliseconds                   uptime{1500};
        std::filesystem::path                       configPath{"/etc/app.conf"};
    };

    void Run() {
        Bundle b{};
        b.comment = "ready";
        b.payload = std::string{"ok"};

        json j = ToJson(b);
        std::println("[5] mixed standard-library types:\n{}\n", j.dump(2));
        std::println("[5] note: optional → null when empty; variant → active\n"
                     "        alternative; pair / tuple → JSON array; map<string,V>\n"
                     "        → JSON object; chrono → ns count; path → UTF-8.\n\n");
    }

} // namespace Demo5

// =============================================================================
// 6.  EmitDefault{false} — skip value-init members for compact output
// =============================================================================

namespace Demo6 {

    struct Config {
        [[= An::EmitDefault{false}]] int         maxRetries = 0;
        [[= An::EmitDefault{false}]] std::string overrideHost;
        std::string                              required = "always-emitted";
    };

    void Run() {
        Config c{};
        std::println("[6] compact (defaults skipped):\n{}\n", ToJson(c).dump(2));

        c.maxRetries   = 5;
        c.overrideHost = "alt.example";
        std::println("[6] populated:\n{}\n\n", ToJson(c).dump(2));
    }

} // namespace Demo6

// =============================================================================
// 7.  Custom hooks — member ToJson / FromJson always wins
// =============================================================================

namespace Demo7 {

    // Imagine an opaque ID we want to wire-encode as `{ "id_hex": "..." }`
    // rather than the default integer field dump.
    struct OpaqueId {
        uint64_t bits = 0;

        [[nodiscard]] json ToJson() const {
            char buf[19];
            std::snprintf(buf, sizeof buf, "0x%016llX",
                          static_cast<unsigned long long>(bits));
            return json{{"id_hex", std::string(buf)}};
        }

        static OpaqueId FromJson(const json& j) {
            OpaqueId out{};
            std::sscanf(j.at("id_hex").get<std::string>().c_str(), "0x%llX",
                        reinterpret_cast<unsigned long long*>(&out.bits));
            return out;
        }
    };

    void Run() {
        OpaqueId id{0xCAFEBABEDEADBEEFULL};
        json     j   = ToJson(id);
        OpaqueId rt  = FromJson<OpaqueId>(j);
        std::println("[7] member hook overrides reflection:\n{}\n", j.dump(2));
        std::println("[7] round-trip preserved bits = 0x{:016X}\n\n", rt.bits);
    }

} // namespace Demo7

// =============================================================================
// 8.  nlohmann::adl_serializer bridge — `json j = v;` works transparently
// =============================================================================

namespace Demo8 {

    struct Vec3 { float x, y, z; };

    void Run() {
        Vec3 v{1.5f, 2.5f, 3.5f};

        // Implicit conversion goes through Mashiro's adl_serializer
        // partial specialisation — no manual to_json needed.
        json j = v;

        // Convenience: we can even use json's operator[] then assign the struct.
        json doc;
        doc["origin"] = Vec3{0, 0, 0};
        doc["target"] = v;

        std::println("[8] adl_serializer bridge:\n{}\n", doc.dump(2));

        Vec3 back = j.template get<Vec3>();
        std::println("[8] back via j.get<Vec3>(): ({}, {}, {})\n\n",
                     back.x, back.y, back.z);
    }

} // namespace Demo8

// =============================================================================
// Driver
// =============================================================================

int main() {
    std::println("════════ Mashiro ToJson — guided tour ════════\n");
    Demo1::Run();
    Demo2::Run();
    Demo3::Run();
    Demo4::Run();
    Demo5::Run();
    Demo6::Run();
    Demo7::Run();
    Demo8::Run();
    std::println("════════ end of tour ════════");
    return 0;
}
