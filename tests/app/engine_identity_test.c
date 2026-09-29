#include "../../src/app/core.h"
#include <stdio.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char hash[65];
        if (!app_engine_sha256(argv[i], hash))
            return 1;
        puts(hash);
    }
    return 0;
}
