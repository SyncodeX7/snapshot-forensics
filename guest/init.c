/*
 * Minimal init for the snapshot-forensics guest.
 *
 * Mounts the pseudo filesystems, prints its PID, forks a few workers that keep
 * touching private memory, and then idles.  The worker names (snapw0..snapw3)
 * are visible in Volatility's linux.pslist output, which makes it easy to
 * prove that a snapshot really came from SMM and not from the host dump.
 *
 * Build: gcc -static -Os -o init init.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void
ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) != 0 && errno != EEXIST)
        perror(path);
}

char g_canary_token[] = "CONFIDENTIAL_CANARY_TOKEN=SMM_FIRMWARE_SNAPSHOT_FORENSICS_2026_RESEARCH";
volatile char *g_canary_ptr = g_canary_token;

static void
worker(int id, int write_heavy)
{
    char name[16];
    size_t size = (size_t)(6 + id) * 1024 * 1024;
    unsigned char *buf;
    unsigned long iter = 0;

    snprintf(name, sizeof(name), "snapw%d", id);
    prctl(PR_SET_NAME, name, 0, 0, 0);

    buf = malloc(size);
    if (buf == NULL)
        _exit(2);
    memset(buf, 0x40 + id, size);

    for (;;) {
        size_t off;
        size_t step = write_heavy ? 512 : 4096;
        for (off = 0; off < size; off += step)
            buf[off] = (unsigned char)(iter + off);
        iter++;
        if (write_heavy)
            usleep(50000); // 50 ms loop for write-heavy stress
        else
            sleep(1);
    }
}

int
main(void)
{
    int fd;
    int i;
    int crash_after = 0;
    int write_heavy = 0;
    char cmdline[1024];

    ensure_dir("/proc", 0555);
    ensure_dir("/sys", 0555);
    ensure_dir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }

    // Allocate and anchor forensic canary token in memory (for forensic artifact verification)
    g_canary_ptr = strdup(g_canary_token);
    __asm__ volatile("" : : "g"(g_canary_ptr), "g"(g_canary_token) : "memory");

    // Read command line options
    memset(cmdline, 0, sizeof(cmdline));
    fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
    }

    if (strstr(cmdline, "workload=write_heavy"))
        write_heavy = 1;

    char *p = strstr(cmdline, "crash_after=");
    if (p)
        crash_after = atoi(p + 12);

    printf("\nSNAPSHOT-GUEST: booted, init pid=%d write_heavy=%d crash_after=%d\n",
           getpid(), write_heavy, crash_after);
    fflush(stdout);

    for (i = 0; i < 4; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            worker(i, write_heavy);
            _exit(0);
        }
        if (pid > 0) {
            printf("SNAPSHOT-GUEST: worker %d pid=%d\n", i, pid);
            fflush(stdout);
        }
    }

    unsigned long elapsed = 0;
    for (;;) {
        sleep(2);
        elapsed += 2;
        printf("SNAPSHOT-GUEST: alive elapsed=%lu\n", elapsed);
        fflush(stdout);

        if (crash_after > 0 && elapsed >= (unsigned long)crash_after) {
            printf("SNAPSHOT-GUEST: DELIBERATE KERNEL PANIC TRIGGERED via SysRq (RQ1 crash forensics test)!\n");
            fflush(stdout);
            int sfd = open("/proc/sysrq-trigger", O_WRONLY);
            if (sfd >= 0) {
                write(sfd, "c\n", 2);
                close(sfd);
            }
        }
    }
}
