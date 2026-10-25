// Records every call reaching the engine's VReport (rva 0x0063c340) while a capture is active, instead of
// letting the engine handle it. Both the original engine functions and reconstructions end up there, so
// comparing two captures compares the reports two implementations would have made.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../host/hooks.h"

namespace tests {

struct ReportCall {
    int kind;
    std::string tag;
    std::string file;
    int line;
    std::string expr;
    std::string message;  // fmt expanded with its arguments ("" when fmt is null)
};

bool operator==(const ReportCall& a, const ReportCall& b);

class ReportCapture {
public:
    // While alive, VReport is detoured to a recorder returning `scripted_result`.
    explicit ReportCapture(int scripted_result);
    ~ReportCapture();

    bool active() const;
    std::vector<ReportCall> Take();

private:
    std::unique_ptr<protore::TestDetour> detour_;
};

}  // namespace tests
