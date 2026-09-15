#include <cstdio>
#include <unistd.h>

namespace {

__attribute__((constructor)) void kirkware_load_test_on_load() {
    std::fprintf(stderr,
                 "[kirkware load test] module loaded in pid %ld\n",
                 static_cast<long>(::getpid()));
    std::fflush(stderr);
}

__attribute__((destructor)) void kirkware_load_test_on_unload() {
    std::fprintf(stderr,
                 "[kirkware load test] module unloaded from pid %ld\n",
                 static_cast<long>(::getpid()));
    std::fflush(stderr);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
const char* kirkware_load_test_identity() {
    return "kirkware cooperative load-test module v1";
}
