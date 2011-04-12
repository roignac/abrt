/*
    CCpp.cpp

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <set>
#include <iomanip>
//#include <nss.h>
//#include <sechash.h>
#include "abrtlib.h"
#include "strbuf.h"
#include "CCpp.h"
#include "abrt_exception.h"
#include "debug_dump.h"
#include "comm_layer_inner.h"
#if 0
    #include "Polkit.h"
#endif
#include "CCpp_sha1.h"
#include "../btparser/backtrace.h"
#include "../btparser/frame.h"
#include "../btparser/location.h"

using namespace std;

#define CORE_PATTERN_IFACE      "/proc/sys/kernel/core_pattern"
/*
 * %s - signal number
 * %c - ulimit -c value
 * %p - pid
 * %u - uid
 * %g - gid
 * %t - UNIX time of dump
 * %h - hostname
 * %e - executable filename
 * %% - output one "%"
 */
#define CORE_PATTERN            "|"CCPP_HOOK_PATH" "DEBUG_DUMPS_DIR" %s %c %p %u %g %t %h %e"

#define CORE_PIPE_LIMIT_IFACE   "/proc/sys/kernel/core_pipe_limit"
/* core_pipe_limit specifies how many dump_helpers might run at the same time
 * 0 - means unlimited, but the it's not guaranteed that /proc/<pid>
 *     of crashing process will be available for dump_helper
 * 4 - means that 4 dump_helpers can run at the same time, which should be enough
 *     for ABRT, we can miss some crashes, but what are the odds that more processes
 *     crash at the same time? This value has been recommended by nhorman
 */
#define CORE_PIPE_LIMIT "4"

#define DEBUGINFO_CACHE_DIR     LOCALSTATEDIR"/cache/abrt-di"

CAnalyzerCCpp::CAnalyzerCCpp() :
    m_bBacktrace(true),
    m_bBacktraceRemotes(false),
    m_bMemoryMap(false),
    m_bInstallDebugInfo(true),
    m_nDebugInfoCacheMB(4000)
{}

static string create_hash(const char *pInput)
{
    unsigned int len;

#if 0
{
    char hash_str[SHA1_LENGTH*2 + 1];
    unsigned char hash[SHA1_LENGTH];
    HASHContext *hc;
    hc = HASH_Create(HASH_AlgSHA1);
    if (!hc)
    {
        error_msg_and_die("HASH_Create(HASH_AlgSHA1) failed"); /* paranoia */
    }
    HASH_Begin(hc);
    HASH_Update(hc, (const unsigned char*)pInput, strlen(pInput));
    HASH_End(hc, hash, &len, sizeof(hash));
    HASH_Destroy(hc);

    char *d = hash_str;
    unsigned char *s = hash;
    while (len)
    {
        *d++ = "0123456789abcdef"[*s >> 4];
        *d++ = "0123456789abcdef"[*s & 0xf];
        s++;
        len--;
    }
    *d = '\0';
//log("hash1:%s str:'%s'", hash_str, pInput);
}
#endif

    char hash_str[SHA1_RESULT_LEN*2 + 1];
    unsigned char hash2[SHA1_RESULT_LEN];
    sha1_ctx_t sha1ctx;
    sha1_begin(&sha1ctx);
    sha1_hash(pInput, strlen(pInput), &sha1ctx);
    sha1_end(hash2, &sha1ctx);
    len = SHA1_RESULT_LEN;

    char *d = hash_str;
    unsigned char *s = hash2;
    while (len)
    {
        *d++ = "0123456789abcdef"[*s >> 4];
        *d++ = "0123456789abcdef"[*s & 0xf];
        s++;
        len--;
    }
    *d = '\0';
//log("hash2:%s str:'%s'", hash_str, pInput);

    return hash_str;
}

/* Returns status. See `man 2 wait` for status information. */
static int ExecVP(char **pArgs, uid_t uid, int redirect_stderr, string& pOutput)
{
    /* Nuke everything which may make setlocale() switch to non-POSIX locale:
     * we need to avoid having gdb output in some obscure language.
     */
    static const char *const unsetenv_vec[] = {
        "LANG",
        "LC_ALL",
        "LC_COLLATE",
        "LC_CTYPE",
        "LC_MESSAGES",
        "LC_MONETARY",
        "LC_NUMERIC",
        "LC_TIME",
        NULL
    };

    int flags = EXECFLG_INPUT_NUL | EXECFLG_OUTPUT | EXECFLG_SETGUID | EXECFLG_SETSID | EXECFLG_QUIET;
    if (redirect_stderr)
        flags |= EXECFLG_ERR2OUT;
    VERB1 flags &= ~EXECFLG_QUIET;

    int pipeout[2];
    pid_t child = fork_execv_on_steroids(flags, pArgs, pipeout, (char**)unsetenv_vec, /*dir:*/ NULL, uid);

    /* We use this function to run gdb and unstrip. Bugs in gdb or corrupted
     * coredumps were observed to cause gdb to enter infinite loop.
     * Therefore we have a (largish) timeout, after which we kill the child.
     */
    int t = time(NULL); /* int is enough, no need to use time_t */
    int endtime = t + 60;
    while (1)
    {
        int timeout = endtime - t;
        if (timeout < 0)
        {
            kill(child, SIGKILL);
            pOutput += "\nTimeout exceeded: 60 second, killing ";
            pOutput += pArgs[0];
            pOutput += "\n";
            break;
        }

        /* We don't check poll result - checking read result is enough */
        struct pollfd pfd;
        pfd.fd = pipeout[0];
        pfd.events = POLLIN;
        poll(&pfd, 1, timeout * 1000);

        char buff[1024];
        int r = read(pipeout[0], buff, sizeof(buff) - 1);
        if (r <= 0)
            break;
        buff[r] = '\0';
        pOutput += buff;
        t = time(NULL);
    }
    close(pipeout[0]);

    int status;
    waitpid(child, &status, 0); /* prevent having zombie child process */

    return status;
}

static void GetBacktrace(const char *pDebugDumpDir,
                         const char *pDebugInfoDirs,
                         string& pBacktrace)
{
    update_client(_("Generating backtrace"));

    string UID;
    string executable;
    {
        CDebugDump dd;
        dd.Open(pDebugDumpDir);
        dd.LoadText(FILENAME_EXECUTABLE, executable);
        dd.LoadText(CD_UID, UID);
    }

    // Workaround for
    // http://sourceware.org/bugzilla/show_bug.cgi?id=9622
    unsetenv("TERM");
    // This is not necessary, and was observed to cause
    // environmant corruption (because we run in a thread?):
    //putenv((char*)"TERM=dumb");

    char *args[21];
    args[0] = (char*)"gdb";
    args[1] = (char*)"-batch";

    // when/if gdb supports it:
    // (https://bugzilla.redhat.com/show_bug.cgi?id=528668):
    args[2] = (char*)"-ex";
    string dfd = "set debug-file-directory /usr/lib/debug";
    const char *p = pDebugInfoDirs;
    while (1)
    {
        const char *colon_or_nul = strchrnul(p, ':');
        dfd += ':';
        dfd.append(p, colon_or_nul - p);
        dfd += "/usr/lib/debug";
        if (*colon_or_nul != ':')
            break;
        p = colon_or_nul + 1;
    }
    args[3] = (char*)dfd.c_str();

    /* "file BINARY_FILE" is needed, without it gdb cannot properly
     * unwind the stack. Currently the unwind information is located
     * in .eh_frame which is stored only in binary, not in coredump
     * or debuginfo.
     *
     * Fedora GDB does not strictly need it, it will find the binary
     * by its build-id.  But for binaries either without build-id
     * (= built on non-Fedora GCC) or which do not have
     * their debuginfo rpm installed gdb would not find BINARY_FILE
     * so it is still makes sense to supply "file BINARY_FILE".
     *
     * Unfortunately, "file BINARY_FILE" doesn't work well if BINARY_FILE
     * was deleted (as often happens during system updates):
     * gdb uses specified BINARY_FILE
     * even if it is completely unrelated to the coredump.
     * See https://bugzilla.redhat.com/show_bug.cgi?id=525721
     *
     * TODO: check mtimes on COREFILE and BINARY_FILE and not supply
     * BINARY_FILE if it is newer (to at least avoid gdb complaining).
     */
    args[4] = (char*)"-ex";
    string file = ssprintf("file %s", executable.c_str());
    args[5] = (char*)file.c_str();

    args[6] = (char*)"-ex";
    string corefile = ssprintf("core-file %s/"FILENAME_COREDUMP, pDebugDumpDir);
    args[7] = (char*)corefile.c_str();

    args[8] = (char*)"-ex";
    /*args[9] = ... see below */
    args[10] = (char*)"-ex";
    args[11] = (char*)"info sharedlib";
    /* glibc's abort() stores its message in __abort_msg variable */
    args[12] = (char*)"-ex";
    args[13] = (char*)"print (char*)__abort_msg";
    args[14] = (char*)"-ex";
    args[15] = (char*)"print (char*)__glib_assert_msg";
    args[16] = (char*)"-ex";
    args[17] = (char*)"info registers";
    args[18] = (char*)"-ex";
    args[19] = (char*)"disassemble";
    args[20] = NULL;

    /* Get the backtrace, but try to cap its size */
    /* Limit bt depth. With no limit, gdb sometimes OOMs the machine */
    unsigned bt_depth = 2048;
    const char *thread_apply_all = "thread apply all ";
    const char *full = " full";
    while (1)
    {
        string cmd = ssprintf("%sbacktrace %u%s", thread_apply_all, bt_depth, full);
        args[9] = (char*)cmd.c_str();
        pBacktrace = "";
        ExecVP(args, xatoi_u(UID.c_str()), /*redirect_stderr:*/ 1, pBacktrace);
        if (bt_depth <= 64 || pBacktrace.size() < 256*1024)
            return;
        bt_depth /= 2;
        if (bt_depth <= 64 && thread_apply_all[0] != '\0')
        {
            /* This program likely has gazillion threads, dont try to bt them all */
            bt_depth = 256;
            thread_apply_all = "";
        }
        if (bt_depth <= 64 && full[0] != '\0')
        {
            /* Looks like there are gigantic local structures or arrays, disable "full" bt */
            bt_depth = 256;
            full = "";
        }
    }
}

static void GetIndependentBuildIdPC(const char *unstrip_n_output,
                                    string& pIndependentBuildIdPC)
{
    // lines look like this:
    // 0x400000+0x209000 23c77451cf6adff77fc1f5ee2a01d75de6511dda@0x40024c - - [exe]
    // 0x400000+0x209000 ab3c8286aac6c043fd1bb1cc2a0b88ec29517d3e@0x40024c /bin/sleep /usr/lib/debug/bin/sleep.debug [exe]
    // 0x7fff313ff000+0x1000 389c7475e3d5401c55953a425a2042ef62c4c7df@0x7fff313ff2f8 . - linux-vdso.so.1
    const char *line = unstrip_n_output;
    while (*line)
    {
        const char *eol = strchrnul(line, '\n');
        const char *plus = (char*)memchr(line, '+', eol - line);
        if (plus)
        {
            while (++plus < eol && *plus != '@')
            {
                if (!isspace(*plus))
                {
                    pIndependentBuildIdPC += *plus;
                }
            }
        }
        if (*eol != '\n') break;
        line = eol + 1;
    }
}

static string run_unstrip_n(const char *pDebugDumpDir)
{
    string UID;
    {
        CDebugDump dd;
        dd.Open(pDebugDumpDir);
        dd.LoadText(CD_UID, UID);
    }

    char* args[4];
    args[0] = (char*)"eu-unstrip";
    args[1] = xasprintf("--core=%s/"FILENAME_COREDUMP, pDebugDumpDir);
    args[2] = (char*)"-n";
    args[3] = NULL;

    string output;
    ExecVP(args, xatoi_u(UID.c_str()), /*redirect_stderr:*/ 0, output);

    free(args[1]);

    return output;
}

/* Needs gdb feature from here: https://bugzilla.redhat.com/show_bug.cgi?id=528668
 * It is slated to be in F12/RHEL6.
 */
static void InstallDebugInfos(const char *pDebugDumpDir,
                              const char *debuginfo_dirs,
                              string& build_ids)
{
    update_client(_("Starting the debuginfo installation"));

    int pipeout[2]; //TODO: can we use ExecVP?
    xpipe(pipeout);

    fflush(NULL);
    pid_t child = fork();
    if (child < 0)
    {
        /*close(pipeout[0]); - why bother */
        /*close(pipeout[1]); */
        perror_msg_and_die("fork");
    }
    if (child == 0)
    {
        close(pipeout[0]);
        xmove_fd(pipeout[1], STDOUT_FILENO);
        xmove_fd(xopen("/dev/null", O_RDONLY), STDIN_FILENO);

        char *coredump = xasprintf("%s/"FILENAME_COREDUMP, pDebugDumpDir);
        /* SELinux guys are not happy with /tmp, using /var/run/abrt */
        char *tempdir = xasprintf(LOCALSTATEDIR"/run/abrt/tmp-%lu-%lu", (long)getpid(), (long)time(NULL));
        /* log() goes to stderr/syslog, it's ok to use it here */
        VERB1 log("Executing: %s %s %s %s", "abrt-debuginfo-install", coredump, tempdir, debuginfo_dirs);
        /* We want parent to see errors in the same stream */
        xdup2(STDOUT_FILENO, STDERR_FILENO);
        execlp("abrt-debuginfo-install", "abrt-debuginfo-install", coredump, tempdir, debuginfo_dirs, NULL);
        perror_msg("Can't execute '%s'", "abrt-debuginfo-install");
        /* Serious error (1 means "some debuginfos not found") */
        exit(2);
    }

    close(pipeout[1]);

    FILE *pipeout_fp = fdopen(pipeout[0], "r");
    if (pipeout_fp == NULL) /* never happens */
    {
        close(pipeout[0]);
        waitpid(child, NULL, 0);
        return;
    }

    /* With 126 debuginfos I've seen lines 9k+ chars long...
     * yet, having it truly unlimited is bad too,
     * therefore we are using LARGE, but still limited buffer.
     */
    char *buff = (char*) xmalloc(64*1024);
    while (fgets(buff, 64*1024, pipeout_fp))
    {
        strchrnul(buff, '\n')[0] = '\0';

        if (strncmp(buff, "MISSING:", 8) == 0)
        {
            build_ids += "Debuginfo absent: ";
            build_ids += buff + 8;
            build_ids += "\n";
            continue;
        }

        const char *p = buff;
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p)
        {
            VERB1 log("%s", buff);
            update_client("%s", buff);
        }
    }
    free(buff);
    fclose(pipeout_fp);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        continue;
    if (WIFEXITED(status))
    {
        if (WEXITSTATUS(status) > 1)
            error_msg("%s exited with %u", "abrt-debuginfo-install", (int)WEXITSTATUS(status));
    }
    else
    {
        error_msg("%s killed by signal %u", "abrt-debuginfo-install", (int)WTERMSIG(status));
    }
}

static double get_dir_size(const char *dirname,
                           string *worst_file,
                           double *maxsz)
{
    DIR *dp = opendir(dirname);
    if (dp == NULL)
        return 0;

    struct dirent *ep;
    struct stat stats;
    double size = 0;
    while ((ep = readdir(dp)) != NULL)
    {
        if (dot_or_dotdot(ep->d_name))
            continue;
        string dname = concat_path_file(dirname, ep->d_name);
        if (lstat(dname.c_str(), &stats) != 0)
            continue;
        if (S_ISDIR(stats.st_mode))
        {
            double sz = get_dir_size(dname.c_str(), worst_file, maxsz);
            size += sz;
        }
        else if (S_ISREG(stats.st_mode))
        {
            double sz = stats.st_size;
            size += sz;

            if (worst_file)
            {
                /* Calculate "weighted" size and age
                 * w = sz_kbytes * age_mins */
                sz /= 1024;
                long age = (time(NULL) - stats.st_mtime) / 60;
                if (age > 0)
                    sz *= age;

                if (sz > *maxsz)
                {
                    *maxsz = sz;
                    *worst_file = dname;
                }
            }
        }
    }
    closedir(dp);
    return size;
}

static void trim_debuginfo_cache(unsigned max_mb)
{
    while (1)
    {
        string worst_file;
        double maxsz = 0;
        double cache_sz = get_dir_size(DEBUGINFO_CACHE_DIR, &worst_file, &maxsz);
        if (cache_sz / (1024 * 1024) < max_mb)
            break;
        VERB1 log("%s is %.0f bytes (over %u MB), deleting '%s'",
                DEBUGINFO_CACHE_DIR, cache_sz, max_mb, worst_file.c_str());
        if (unlink(worst_file.c_str()) != 0)
            perror_msg("Can't unlink '%s'", worst_file.c_str());
    }
}

string CAnalyzerCCpp::GetLocalUUID(const char *pDebugDumpDir)
{
    string executable;
    string package;
    {
        CDebugDump dd;
        dd.Open(pDebugDumpDir);
        dd.LoadText(FILENAME_EXECUTABLE, executable);
        dd.LoadText(FILENAME_PACKAGE, package);
    }

    string unstrip_n_output = run_unstrip_n(pDebugDumpDir);
    string independentBuildIdPC;
    GetIndependentBuildIdPC(unstrip_n_output.c_str(), independentBuildIdPC);

    /* package variable has "firefox-3.5.6-1.fc11[.1]" format */
    /* Remove distro suffix and maybe least significant version number */
    char *trimmed_package = xstrdup(package.c_str());
    char *p = trimmed_package;
    while (*p)
    {
        if (*p == '.' && (p[1] < '0' || p[1] > '9'))
        {
            /* We found "XXXX.nondigitXXXX", trim this part */
            *p = '\0';
            break;
        }
        p++;
    }
    char *first_dot = strchr(trimmed_package, '.');
    if (first_dot)
    {
        char *last_dot = strrchr(first_dot, '.');
        if (last_dot != first_dot)
        {
            /* There are more than one dot: "1.2.3"
             * Strip last part, we don't want to distinquish crashes
             * in packages which differ only by minor release number.
             */
            *last_dot = '\0';
        }
    }
    string hash_str = trimmed_package + executable + independentBuildIdPC;
    free(trimmed_package);
    return create_hash(hash_str.c_str());
}

string CAnalyzerCCpp::GetGlobalUUID(const char *pDebugDumpDir)
{
    CDebugDump dd;
    dd.Open(pDebugDumpDir);
    if (dd.Exist(FILENAME_GLOBAL_UUID))
    {
        string uuid;
        dd.LoadText(FILENAME_GLOBAL_UUID, uuid);
        return uuid;
    }
    else
        return "";
}
#if 0
static bool DebuginfoCheckPolkit(uid_t uid)
{
    fflush(NULL);
    int child_pid = fork();
    if (child_pid < 0)
    {
        perror_msg_and_die("fork");
    }
    if (child_pid == 0)
    {
        //child
        xsetreuid(uid, uid);
        PolkitResult result = polkit_check_authorization(getpid(),
                 "org.fedoraproject.abrt.install-debuginfos");
        exit(result != PolkitYes); //exit 1 (failure) if not allowed
    }

    //parent
    int status;
    if (waitpid(child_pid, &status, 0) > 0
     && WIFEXITED(status)
     && WEXITSTATUS(status) == 0
    ) {
        return true; //authorization OK
    }
    log("UID %d is not authorized to install debuginfos", uid);
    return false;
}
#endif

void CAnalyzerCCpp::CreateReport(const char *pDebugDumpDir, int force)
{
    string component, executable, UID;

    CDebugDump dd;
    dd.Open(pDebugDumpDir);

    /* Skip remote crashes. */
    if (dd.Exist(FILENAME_REMOTE))
    {
        std::string remote_str;
        dd.LoadText(FILENAME_REMOTE, remote_str);
        bool remote = (remote_str.find('1') != std::string::npos);
        if (remote && !m_bBacktraceRemotes)
            return;
    }

    if (!m_bBacktrace)
        return;

    if (!force)
    {
        bool bt_exists = dd.Exist(FILENAME_BACKTRACE);
        if (bt_exists)
            return; /* backtrace already exists */
    }

    dd.LoadText(FILENAME_COMPONENT, component);
    dd.LoadText(FILENAME_EXECUTABLE, executable);
    dd.LoadText(CD_UID, UID);
    dd.Close(); /* do not keep dir locked longer than needed */

    string build_ids;
    if (m_bInstallDebugInfo)
    {
        if (m_nDebugInfoCacheMB > 0)
            trim_debuginfo_cache(m_nDebugInfoCacheMB);
        InstallDebugInfos(pDebugDumpDir, m_sDebugInfoDirs.c_str(), build_ids);
    }
    else
        VERB1 log(_("Skipping the debuginfo installation"));

    /* Create and store backtrace. */
    string backtrace_str;
    GetBacktrace(pDebugDumpDir, m_sDebugInfoDirs.c_str(), backtrace_str);
    dd.Open(pDebugDumpDir);
    dd.SaveText(FILENAME_BACKTRACE, (backtrace_str + build_ids).c_str());

    if (m_bMemoryMap)
        dd.SaveText(FILENAME_MEMORYMAP, "memory map of the crashed C/C++ application, not implemented yet");

    /* Compute and store backtrace hash. */
    struct btp_location location;
    btp_location_init(&location);
    char *backtrace_str_ptr = (char*)backtrace_str.c_str();
    struct btp_backtrace *backtrace = btp_backtrace_parse(&backtrace_str_ptr, &location);
    if (!backtrace)
    {
        VERB1 log(_("Backtrace parsing failed for %s"), pDebugDumpDir);
        VERB1 log("%d:%d: %s", location.line, location.column, location.message);
        /* If the parser failed, compute the UUID from the executable and
         * component.  This is not supposed to happen often.
         */
        struct strbuf *emptybt = strbuf_new();
        strbuf_prepend_str(emptybt, executable.c_str());
        strbuf_prepend_str(emptybt, component.c_str());
        VERB3 log("Generating duphash: %s", emptybt->buf);
        string hash_str = create_hash(emptybt->buf);
        dd.SaveText(FILENAME_GLOBAL_UUID, hash_str.c_str());

        /* Other parts of ABRT assume that if no rating is available,
         * it is ok to allow reporting of the bug. To be sure no bad
         * backtrace is reported, rate the backtrace with the lowest
         * rating.
         */
        dd.SaveText(FILENAME_RATING, "0");

        strbuf_free(emptybt);
        dd.Close();
        return;
    }

    /* Compute duplication hash. */
    char *str_hash_core = btp_backtrace_get_duplication_hash(backtrace);
    struct strbuf *str_hash = strbuf_new();
    strbuf_append_str(str_hash, component.c_str());
    strbuf_append_str(str_hash, str_hash_core);
    VERB3 log("Generating duphash: %s", str_hash->buf);
    string hash_str = create_hash(str_hash->buf);
    dd.SaveText(FILENAME_GLOBAL_UUID, hash_str.c_str());
    strbuf_free(str_hash);
    free(str_hash_core);

    /* Compute the backtrace rating. */
    float quality = btp_backtrace_quality_complex(backtrace);
    const char *rating;
    if (quality < 0.6f)
        rating = "0";
    else if (quality < 0.7f)
        rating = "1";
    else if (quality < 0.8f)
        rating = "2";
    else if (quality < 0.9f)
        rating = "3";
    else
        rating = "4";
    dd.SaveText(FILENAME_RATING, rating);

    /* Get the function name from the crash frame. */
    struct btp_frame *crash_frame = btp_backtrace_get_crash_frame(backtrace);
    if (crash_frame)
     {
        if (crash_frame->function_name &&
            0 != strcmp(crash_frame->function_name, "??"))
        {
	  dd.SaveText(FILENAME_CRASH_FUNCTION, crash_frame->function_name);
        }
        btp_frame_free(crash_frame);
     }
    btp_backtrace_free(backtrace);
    dd.Close();
}

/*
 this is just a workaround until kernel changes it's behavior
 when handling pipes in core_pattern
*/
#ifdef HOSTILE_KERNEL
#define CORE_SIZE_PATTERN "Max core file size=1:unlimited"
static int isdigit_str(char *str)
{
    do {
        if (*str < '0' || *str > '9')
            return 0;
    } while (*++str);
    return 1;
}

static int set_limits()
{
    DIR *dir = opendir("/proc");
    if (!dir) {
        /* this shouldn't fail, but to be safe.. */
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit_str(ent->d_name))
            continue;

        char limits_name[sizeof("/proc/%s/limits") + sizeof(long)*3];
        snprintf(limits_name, sizeof(limits_name), "/proc/%s/limits", ent->d_name);
        FILE *limits_fp = fopen(limits_name, "r");
        if (!limits_fp) {
            break;
        }

        char line[128];
        char *ulimit_c = NULL;
        while (1) {
            if (fgets(line, sizeof(line)-1, limits_fp) == NULL)
                break;
            if (strncmp(line, "Max core file size", sizeof("Max core file size")-1) == 0) {
                ulimit_c = skip_whitespace(line + sizeof("Max core file size")-1);
                skip_non_whitespace(ulimit_c)[0] = '\0';
                break;
            }
        }
        fclose(limits_fp);
        if (!ulimit_c || ulimit_c[0] != '0' || ulimit_c[1] != '\0') {
            /*process has nonzero ulimit -c, so need to modify it*/
            continue;
        }
        /* echo -n 'Max core file size=1:unlimited' >/proc/PID/limits */
        int fd = open(limits_name, O_WRONLY);
        if (fd >= 0) {
            errno = 0;
            /*full_*/
            ssize_t n = write(fd, CORE_SIZE_PATTERN, sizeof(CORE_SIZE_PATTERN)-1);
            if (n < sizeof(CORE_SIZE_PATTERN)-1)
                log("warning: can't write core_size limit to: %s", limits_name);
            close(fd);
        }
        else
        {
            log("warning: can't open %s for writing", limits_name);
        }
    }
    closedir(dir);
    return 0;
}
#endif /* HOSTILE_KERNEL */

void CAnalyzerCCpp::Init()
{
    FILE *fp = fopen(CORE_PATTERN_IFACE, "r");
    if (fp)
    {
        char line[PATH_MAX];
        if (fgets(line, sizeof(line), fp))
            m_sOldCorePattern = line;
        fclose(fp);
    }
    if (m_sOldCorePattern[0] == '|')
    {
        if (strncmp(m_sOldCorePattern.c_str(), CORE_PATTERN, strlen(CORE_PATTERN)) == 0)
        {
            log("warning: %s already contains %s, "
                "did abrt daemon crash recently?",
                CORE_PATTERN_IFACE, CORE_PATTERN);
            /* There is no point in "restoring" CORE_PATTERN_IFACE
             * to CORE_PATTERN on exit. Will restore to a default value:
             */
            m_sOldCorePattern = "core";
        } else {
            log("warning: %s was already set to run a crash analyser (%s), "
                "abrt may interfere with it",
                CORE_PATTERN_IFACE, CORE_PATTERN);
        }
    }
#ifdef HOSTILE_KERNEL
    if (set_limits() != 0)
        log("warning: failed to set core_size limit, ABRT won't detect crashes in"
            "compiled apps");
#endif

    fp = fopen(CORE_PATTERN_IFACE, "w");
    if (fp)
    {
        if (m_sOldCorePattern[0] != '|')
        {
            const char *old = m_sOldCorePattern.c_str();
            unsigned len = strchrnul(old, '\n') - old;
            char *hex_old = (char*)xmalloc(len * 2 + 1);
            bin2hex(hex_old, old, len)[0] = '\0';
            /* Trailing 00 is a sentinel. Decoder in the hook will check it.
             * If it won't see it, then kernel has truncated the argument:
             */
            char *pattern = xasprintf("%s %s00", CORE_PATTERN, hex_old);
            //log("old:'%s'->'%s'", old, hex_old);
            //log("pattern:'%s'", pattern);

            fputs(pattern, fp);

            free(pattern);
            free(hex_old);
        }
        else
        {
            fputs(CORE_PATTERN, fp);
        }
        fclose(fp);
    }

    /* read the core_pipe_limit and change it if it's == 0
     * otherwise the abrt-hook-ccpp won't be able to read /proc/<pid>
     * of the crashing process
     */
    fp = fopen(CORE_PIPE_LIMIT_IFACE, "r");
    if (fp)
    {
        /* we care only about the first char, if it's
         * not '0' then we don't have to change it,
         * because it means that it's already != 0
         */
        char pipe_limit[2];
        if (!fgets(pipe_limit, sizeof(pipe_limit), fp))
            pipe_limit[0] = '1'; /* not 0 */
        fclose(fp);
        if (pipe_limit[0] == '0')
        {
            fp = fopen(CORE_PIPE_LIMIT_IFACE, "w");
            if (fp)
            {
                fputs(CORE_PIPE_LIMIT, fp);
                fclose(fp);
            }
            else
            {
                log("warning: failed to set core_pipe_limit, ABRT won't detect"
                    "crashes in compiled apps if kernel > 2.6.31");
            }
        }
    }
}

void CAnalyzerCCpp::DeInit()
{
    /* no need to restore the core_pipe_limit, because it's only used
       when there is s pipe in core_pattern
    */
    FILE *fp = fopen(CORE_PATTERN_IFACE, "w");
    if (fp)
    {
        fputs(m_sOldCorePattern.c_str(), fp);
        fclose(fp);
    }
}

void CAnalyzerCCpp::SetSettings(const map_plugin_settings_t& pSettings)
{
    m_pSettings = pSettings;

    map_plugin_settings_t::const_iterator end = pSettings.end();
    map_plugin_settings_t::const_iterator it;
    it = pSettings.find("Backtrace");
    if (it != end)
    {
        m_bBacktrace = string_to_bool(it->second.c_str());
    }
    it = pSettings.find("BacktraceRemotes");
    if (it != end)
    {
        m_bBacktraceRemotes = string_to_bool(it->second.c_str());
    }
    it = pSettings.find("MemoryMap");
    if (it != end)
    {
        m_bMemoryMap = string_to_bool(it->second.c_str());
    }
    it = pSettings.find("DebugInfo");
    if (it != end)
    {
        m_sDebugInfo = it->second;
    }
    it = pSettings.find("DebugInfoCacheMB");
    if (it != end)
    {
        m_nDebugInfoCacheMB = xatou(it->second.c_str());
    }
    it = pSettings.find("InstallDebugInfo");
    if (it == end) //compat, remove after 0.0.11
        it = pSettings.find("InstallDebuginfo");
    if (it != end)
    {
        m_bInstallDebugInfo = string_to_bool(it->second.c_str());
    }
    m_sDebugInfoDirs = DEBUGINFO_CACHE_DIR;
    it = pSettings.find("ReadonlyLocalDebugInfoDirs");
    if (it != end)
    {
        m_sDebugInfoDirs += ':';
        m_sDebugInfoDirs += it->second;
    }
}

//ok to delete?
//const map_plugin_settings_t& CAnalyzerCCpp::GetSettings()
//{
//    m_pSettings["MemoryMap"] = m_bMemoryMap ? "yes" : "no";
//    m_pSettings["DebugInfo"] = m_sDebugInfo;
//    m_pSettings["DebugInfoCacheMB"] = to_string(m_nDebugInfoCacheMB);
//    m_pSettings["InstallDebugInfo"] = m_bInstallDebugInfo ? "yes" : "no";
//
//    return m_pSettings;
//}

PLUGIN_INFO(ANALYZER,
            CAnalyzerCCpp,
            "CCpp",
            "0.0.1",
            _("Analyzes crashes in C/C++ programs"),
            "zprikryl@redhat.com",
            "https://fedorahosted.org/abrt/wiki",
            "");