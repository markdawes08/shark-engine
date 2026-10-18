#include "report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

using shark::probe::Json;

TEST_CASE("Probe reports preserve unsigned inventory values and JSON types", "[graphics-probe]") {
    const Json report(Json::Object{
        {"array", Json::Array{Json{}, false, true, 0U, std::numeric_limits<std::uint64_t>::max()}},
        {"empty", Json::Object{}},
        {"status", "NOT_RUN"},
    });
    std::ostringstream stream;
    report.write(stream);
    REQUIRE(stream.str() ==
            R"({"array":[null,false,true,0,18446744073709551615],"empty":{},"status":"NOT_RUN"})");
}

TEST_CASE("Probe reports escape controls and preserve unknown driver bytes", "[graphics-probe]") {
    std::string bytes = "quote\" slash\\ newline\n tab\t";
    bytes.push_back('\0');
    bytes.push_back(static_cast<char>(0x7f));
    bytes.push_back(static_cast<char>(0xff));
    std::ostringstream stream;
    Json(std::move(bytes)).write(stream);
    REQUIRE(stream.str() == R"("quote\" slash\\ newline\u000a tab\u0009\u0000\u007f\u00ff")");
}
