#pragma once

#include <string>

namespace github {

struct SubmitResult {
    bool        ok          = false;
    int         issueNumber = 0;
    std::string issueUrl;
    std::string error;
};

// POST a new issue to the configured repo. `label` is "bug" or
// "enhancement". Blocks for the duration of the HTTPS exchange (a few
// seconds). Caller is expected to release the canvas first if it needs
// the heap; this function does NOT manage the canvas itself because
// the report app already owns SVC_CANVAS lifecycle.
SubmitResult submitIssue(const std::string& title,
                         const std::string& body,
                         const std::string& label);

// True iff CLAWD_GITHUB_PAT was compiled in.
bool hasToken();

}  // namespace github
