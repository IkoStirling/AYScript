#include "AYTest.h"

#include <AYEntity/EntityModule.h>

#include <cstdio>

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    ayt::test::RunOptions options;
    bool help = false;
    if (ayt::test::parseRunOptions(argc, argv, options, help)) return 2;
    if (help || options.list) return ayt::test::runTests("AYScript", argc, argv);

    // AYScript integration tests act as a small engine host. AYEntity's
    // component registry is explicit, so register its component types before
    // any test asks World to create a ScriptComponent storage. Do not call
    // bootstrapEntityCore() here: that would also register the live Entity
    // subsystem and invalidate tests that deliberately exercise the
    // headless/no-Entity-subsystem branch.
    ayt::entity::registerEntityComponents();

    return ayt::test::runTests("AYScript", options);
}
