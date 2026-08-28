#include <clocale>

extern "C" int sage_main(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    if (!std::setlocale(LC_CTYPE, "C.UTF-8")) {
        std::setlocale(LC_CTYPE, "");
    }
    std::setlocale(LC_MESSAGES, "C");
    return sage_main(argc, argv);
}
