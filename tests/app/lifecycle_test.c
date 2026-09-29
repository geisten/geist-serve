#include "../../src/lifecycle.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
    char directory[] = "/tmp/geist-lifecycle-test-XXXXXX";
    assert(mkdtemp(directory));
    struct lifecycle_shared *shared = nullptr;
    int                      fd     = lifecycle_create(directory, 42, &shared);
    assert(fd >= 0 && shared);
    struct stat st;
    assert(!fstat(fd, &st) && st.st_nlink == 0 && (st.st_mode & 077) == 0);
    struct lifecycle_snapshot sample;
    assert(!lifecycle_read(shared, 42, &sample));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(dup2(fd, 4) == 4);
        setenv("GEIST_LIFECYCLE_FD", "4", 1);
        struct lifecycle_shared *owned = lifecycle_inherit();
        assert(owned);
        for (unsigned i = LC_BACKEND; i < LC_PHASES; i++)
            lifecycle_phase(owned, i);
        lifecycle_phase(owned, LC_BACKEND); /* no duplicate overwrite */
        lifecycle_close(&owned);
        assert(!owned);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(lifecycle_read(shared, 42, &sample));
    assert(sample.generation == 42 && sample.process == (uint64_t) child && sample.sequence == 10);
    for (unsigned i = LC_BACKEND; i < LC_PHASES; i++)
        assert(sample.phase_ns[i] >= sample.started_ns &&
               (i == LC_BACKEND || sample.phase_ns[i] >= sample.phase_ns[i - 1]));
    assert(!lifecycle_read(shared, 43, &sample));
    assert(!sample.sequence);
    close(fd);
    lifecycle_close(&shared);
    lifecycle_close(&shared);
    assert(!shared);
    assert(rmdir(directory) == 0);
    unsetenv("GEIST_LIFECYCLE_FD");
    assert(!lifecycle_inherit());
    setenv("GEIST_LIFECYCLE_FD", "5", 1);
    assert(!lifecycle_inherit());
    puts("lifecycle: private unlinked inherited channel, cross-process phases, generations and "
         "cleanup passed");
}
