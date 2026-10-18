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

TEST_CASE("Probe reports preserve finite readback numbers and reject nonfinite values",
          "[graphics-probe]") {
    std::ostringstream stream;
    Json(Json::Array{0.0, -0.25, 1e-5, 2e-3, 1024.5}).write(stream);
    REQUIRE(stream.str() == "[0,-0.25,1.0000000000000001e-05,0.002,1024.5]");
    REQUIRE_THROWS_AS(Json(std::numeric_limits<double>::infinity()), std::invalid_argument);
    REQUIRE_THROWS_AS(Json(-std::numeric_limits<double>::infinity()), std::invalid_argument);
    REQUIRE_THROWS_AS(Json(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}
