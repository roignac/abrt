/*
    MiddleWare.cpp

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
#include "abrtlib.h"
#include "Daemon.h"
#include "Settings.h"
#include "abrt_exception.h"
#include "comm_layer_inner.h"
#include "CommLayerServer.h"
#include "MiddleWare.h"

using namespace std;

/**
 * An instance of CPluginManager. When MiddleWare wants to do something
 * with plugins, it calls the plugin manager.
 * @see PluginManager.h
 */
CPluginManager* g_pPluginManager;


/**
 * Get one crash info. If getting is successful,
 * then crash info is filled.
 * @param dump_dir_name A dump dir containing all necessary data.
 * @param pCrashData A crash info.
 * @return It return results of operation. See mw_result_t.
 */
static mw_result_t FillCrashInfo(const char *dump_dir_name,
                        map_crash_data_t& pCrashData);

/**
 * Transforms a debugdump directory to inner crash
 * report form. This form is used for later reporting.
 * @param dump_dir_name A debugdump dir containing all necessary data.
 * @param pCrashData A created crash report.
 */
static bool DebugDumpToCrashReport(const char *dump_dir_name, map_crash_data_t& pCrashData)
{
    VERB3 log(" DebugDumpToCrashReport('%s')", dump_dir_name);

    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        return false;

    const char *const *v = must_have_files;
    while (*v)
    {
        if (!dd_exist(dd, *v))
        {
            dd_close(dd);
            log("Important file '%s/%s' is missing", dump_dir_name, *v);
            return false;
        }
        v++;
    }

    load_crash_data_from_crash_dump_dir(dd, pCrashData);
    char *events = list_possible_events(dd, NULL, "");
    dd_close(dd);

    add_to_crash_data_ext(pCrashData, CD_EVENTS, CD_SYS, CD_ISNOTEDITABLE, events);
    free(events);

    add_to_crash_data_ext(pCrashData, CD_DUMPDIR, CD_SYS, CD_ISNOTEDITABLE, dump_dir_name);

    return true;
}

static char *do_log_and_update_client(char *log_line, void *param)
{
    VERB1 log("%s", log_line);
    update_client("%s", log_line);
    return log_line;
}

/*
 * Called in two cases:
 * (1) by StartJob dbus call -> CreateReportThread(), in the thread
 * (2) by CreateReport dbus call
 */
mw_result_t CreateCrashReport(const char *dump_dir_name,
                long caller_uid,
                int force,
                map_crash_data_t& pCrashData)
{
    VERB2 log("CreateCrashReport('%s',%ld,result)", dump_dir_name, caller_uid);

    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        return MW_NOENT_ERROR;

    mw_result_t r = MW_OK;

    if (caller_uid != 0) /* not called by root */
    {
        char caller_uid_str[sizeof(long) * 3 + 2];
        sprintf(caller_uid_str, "%ld", caller_uid);

        char *uid = dd_load_text(dd, FILENAME_UID);
        if (strcmp(uid, caller_uid_str) != 0)
        {
            char *inform_all = dd_load_text_ext(dd, FILENAME_INFORMALL, DD_FAIL_QUIETLY);
            bool for_all = string_to_bool(inform_all);
            free(inform_all);
            if (!for_all)
            {
                dd_close(dd);
                error_msg("crash '%s' can't be accessed by user with uid %ld", dump_dir_name, caller_uid);
                r = MW_PERM_ERROR;
                goto ret;
            }
        }
    }
    dd_close(dd);

    try
    {
        struct run_event_state *run_state = new_run_event_state();
        run_state->logging_callback = do_log_and_update_client;
        int res = run_event(run_state, dump_dir_name, force ? "reanalyze" : "analyze");
        free_run_event_state(run_state);
        if (res != 0 && res != -1) /* -1 is "nothing was done", here it is ok */
        {
            r = MW_PLUGIN_ERROR;
            goto ret;
        }

        /* Do a load_crash_data_from_crash_dump_dir from (possibly updated)
         * crash dump dir
         */
        if (!DebugDumpToCrashReport(dump_dir_name, pCrashData))
        {
            error_msg("Error loading crash data");
            r = MW_ERROR;
            goto ret;
        }
    }
    catch (CABRTException& e)
    {
        r = MW_CORRUPTED;
        error_msg("%s", e.what());
        if (e.type() == EXCEP_PLUGIN)
        {
            r = MW_PLUGIN_ERROR;
        }
    }

 ret:
    VERB3 log("CreateCrashReport() returns %d", r);
    return r;
}

void RunAction(const char *pActionDir,
                            const char *pPluginName,
                            const char *pPluginArgs)
{
    CAction* action = g_pPluginManager->GetAction(pPluginName);
    if (!action)
    {
        /* GetAction() already complained */
        return;
    }
    try
    {
        action->Run(pActionDir, pPluginArgs, /*force:*/ 0);
    }
    catch (CABRTException& e)
    {
        error_msg("Execution of '%s' was not successful: %s", pPluginName, e.what());
    }
}

struct logging_state {
    char *last_line;
};

static char *do_log_and_save_line(char *log_line, void *param)
{
    struct logging_state *l_state = (struct logging_state *)param;

    VERB1 log("%s", log_line);
    update_client("%s", log_line);
    free(l_state->last_line);
    l_state->last_line = log_line;
    return NULL;
}

// Do not trust client_report here!
// dbus handler passes it from user without checking
report_status_t Report(const map_crash_data_t& client_report,
                       const vector_string_t& events,
                       const map_map_string_t& settings,
                       long caller_uid)
{
    // Get ID fields
    const char *UID = get_crash_data_item_content_or_NULL(client_report, FILENAME_UID);
    const char *dump_dir_name = get_crash_data_item_content_or_NULL(client_report, CD_DUMPDIR);
    if (!UID || !dump_dir_name)
    {
        throw CABRTException(EXCEP_ERROR, "Report(): UID or DUMPDIR is missing in client's report data");
    }

    // Retrieve corresponding stored record
    map_crash_data_t stored_report;
    mw_result_t r = FillCrashInfo(dump_dir_name, stored_report);
    if (r != MW_OK)
    {
        return report_status_t();
    }

    // Is it allowed for this user to report?
    if (caller_uid != 0   // not called by root
     && strcmp(to_string(caller_uid).c_str(), UID) != 0
    ) {
        const char *inform_all = get_crash_data_item_content_or_NULL(stored_report, FILENAME_INFORMALL);
        if (!inform_all || !string_to_bool(inform_all))
            throw CABRTException(EXCEP_ERROR, "Report(): user with uid %ld can't report crash %s",
                        caller_uid, dump_dir_name);
    }

    // Save comment, "how to reproduce", backtrace
//TODO: we should iterate through stored_report and modify all
//modifiable fields which have new data in client_report
    const char *comment = get_crash_data_item_content_or_NULL(client_report, FILENAME_COMMENT);
    const char *reproduce = get_crash_data_item_content_or_NULL(client_report, FILENAME_REPRODUCE);
    const char *backtrace = get_crash_data_item_content_or_NULL(client_report, FILENAME_BACKTRACE);
    if (comment || reproduce || backtrace)
    {
        struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
        if (dd)
        {
            if (comment)
            {
                dd_save_text(dd, FILENAME_COMMENT, comment);
                add_to_crash_data_ext(stored_report, FILENAME_COMMENT, CD_TXT, CD_ISEDITABLE, comment);
            }
            if (reproduce)
            {
                dd_save_text(dd, FILENAME_REPRODUCE, reproduce);
                add_to_crash_data_ext(stored_report, FILENAME_REPRODUCE, CD_TXT, CD_ISEDITABLE, reproduce);
            }
            if (backtrace)
            {
                dd_save_text(dd, FILENAME_BACKTRACE, backtrace);
                add_to_crash_data_ext(stored_report, FILENAME_BACKTRACE, CD_TXT, CD_ISEDITABLE, backtrace);
            }
            dd_close(dd);
        }
    }

    /* Remove BIN filenames from stored_report if they are not present in client's data */
    map_crash_data_t::const_iterator its = stored_report.begin();
    while (its != stored_report.end())
    {
        if (its->second[CD_TYPE] == CD_BIN)
        {
            std::string key = its->first;
            if (get_crash_data_item_content_or_NULL(client_report, key.c_str()) == NULL)
            {
                /* client does not have it -> does not want it passed to events */
                VERB3 log("Won't report BIN file %s:'%s'", key.c_str(), its->second[CD_CONTENT].c_str());
                its++; /* move off the element we will erase */
                stored_report.erase(key);
                continue;
            }
        }
        its++;
    }

    VERB3 {
        log_map_crash_data(client_report, " client_report");
        log_map_crash_data(stored_report, " stored_report");
    }
#define client_report client_report_must_not_be_used_below

    // Export overridden settings as environment variables
    GList *env_list = NULL;
    map_map_string_t::const_iterator reporter_settings = settings.begin();
    while (reporter_settings != settings.end())
    {
        map_string_t::const_iterator var = reporter_settings->second.begin();
        while (var != reporter_settings->second.end())
        {
            char *s = xasprintf("%s_%s=%s", reporter_settings->first.c_str(), var->first.c_str(), var->second.c_str());
            VERB3 log("Exporting '%s'", s);
            putenv(s);
            env_list = g_list_append(env_list, s);
            var++;
        }
        reporter_settings++;
    }

    // Run events
    bool at_least_one_reporter_succeeded = false;
    report_status_t ret;
    std::string message;
    struct logging_state l_state;
    struct run_event_state *run_state = new_run_event_state();
    run_state->logging_callback = do_log_and_save_line;
    run_state->logging_param = &l_state;
    for (unsigned i = 0; i < events.size(); i++)
    {
        std::string event = events[i];

        l_state.last_line = NULL;
        int r = run_event(run_state, dump_dir_name, event.c_str());
        if (r == -1)
        {
            l_state.last_line = xasprintf("Error: no processing is specified for event '%s'", event.c_str());
        }
        if (r == 0)
        {
            at_least_one_reporter_succeeded = true;
            ret[event].push_back("1"); // REPORT_STATUS_IDX_FLAG
            ret[event].push_back(l_state.last_line ? : "Reporting succeeded"); // REPORT_STATUS_IDX_MSG
            if (message != "")
                message += ";";
            message += (l_state.last_line ? : "Reporting succeeded");
        }
        else
        {
            ret[event].push_back("0");      // REPORT_STATUS_IDX_FLAG
            ret[event].push_back(l_state.last_line ? : "Error in reporting"); // REPORT_STATUS_IDX_MSG
            update_client("Reporting via '%s' was not successful%s%s",
                    event.c_str(),
                    l_state.last_line ? ": " : "",
                    l_state.last_line ? l_state.last_line : ""
            );
        }
        free(l_state.last_line);
    }
    free_run_event_state(run_state);

    // Unexport overridden settings
    for (GList *li = env_list; li; li = g_list_next(li))
    {
        char *s = (char*)li->data;
        /* Need to make a copy: just cutting s at '=' and unsetenv'ing
         * the result would be a bug! s _itself_ is in environment now,
         * we must not modify it there!
         */
        char *name = xstrndup(s, strchrnul(s, '=') - s);
        VERB3 log("Unexporting '%s'", name);
        unsetenv(name);
        free(name);
        free(s);
    }
    g_list_free(env_list);

    // Save reporting results
    if (at_least_one_reporter_succeeded)
    {
        report_status_t::iterator ret_it = ret.begin();
        while (ret_it != ret.end())
        {
//            const string &event = ret_it->first;
//            const vector_string_t &v = ret_it->second;
//            if (v[REPORT_STATUS_IDX_FLAG] == "1")
//            {
// TODO: append to a log of reports done
//                database->SetReportedPerReporter(dump_dir_name, event.c_str(), v[REPORT_STATUS_IDX_MSG].c_str());
//            }
            ret_it++;
        }
        /* Was: database->SetReported(dump_dir_name, message.c_str()); */
        struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
        if (dd)
        {
            dd_save_text(dd, FILENAME_MESSAGE, message.c_str());
            dd_close(dd);
        }
    }

    return ret;
#undef client_report
}

/* We need to share some data between LoadDebugDump and is_crash_a_dup: */
struct cdump_state {
    char *uid;                   /* filled by LoadDebugDump */
    char *uuid;                  /* filled by is_crash_a_dup */
    char *crash_dump_dup_name;   /* filled by is_crash_a_dup */
};

static int is_crash_a_dup(const char *dump_dir_name, void *param)
{
    struct cdump_state *state = (struct cdump_state *)param;

    if (state->uuid)
        return 0; /* we already checked it, don't do it again */

    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        return 0; /* wtf? (error, but will be handled elsewhere later) */
    state->uuid = dd_load_text_ext(dd, FILENAME_UUID,
                DD_FAIL_QUIETLY + DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE
    );
    dd_close(dd);
    if (!state->uuid)
    {
        return 0; /* no uuid (yet), "run_event, please continue iterating" */
    }

    /* Scan crash dumps looking for a dup */
//TODO: explain why this is safe wrt concurrent runs
    DIR *dir = opendir(DEBUG_DUMPS_DIR);
    if (dir != NULL)
    {
        struct dirent *dent;
        while ((dent = readdir(dir)) != NULL)
        {
            if (dot_or_dotdot(dent->d_name))
                continue; /* skip "." and ".." */

            int different;
            char *uid, *uuid;
            char *dump_dir_name2 = concat_path_file(DEBUG_DUMPS_DIR, dent->d_name);

            if (strcmp(dump_dir_name, dump_dir_name2) == 0)
                goto next; /* we are never a dup of ourself */

            dd = dd_opendir(dump_dir_name2, /*flags:*/ 0);
            if (!dd)
                goto next;
            uid = dd_load_text(dd, FILENAME_UID);
            uuid = dd_load_text(dd, FILENAME_UUID);
            dd_close(dd);
            different = strcmp(state->uid, uid) || strcmp(state->uuid, uuid);
            free(uid);
            free(uuid);
            if (different)
                goto next;

            state->crash_dump_dup_name = dump_dir_name2;
            /* "run_event, please stop iterating": */
            return 1;

 next:
            free(dump_dir_name2);
        }
        closedir(dir);
    }

    /* No dup found */
    return 0; /* "run_event, please continue iterating" */
}

static char *do_log(char *log_line, void *param)
{
    VERB1 log("%s", log_line);
    //update_client("%s", log_line);
    return log_line;
}

mw_result_t LoadDebugDump(const char *dump_dir_name,
                          map_crash_data_t& pCrashData)
{
    mw_result_t res;

    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        return MW_ERROR;
    struct cdump_state state;
    state.uid = dd_load_text(dd, FILENAME_UID);
    state.uuid = NULL;
    state.crash_dump_dup_name = NULL;
    char *analyzer = dd_load_text(dd, FILENAME_ANALYZER);
    dd_close(dd);

    res = MW_ERROR;

    /* Run post-create event handler(s) */
    struct run_event_state *run_state = new_run_event_state();
    run_state->post_run_callback = is_crash_a_dup;
    run_state->post_run_param = &state;
    run_state->logging_callback = do_log;
    int r = run_event(run_state, dump_dir_name, "post-create");
    free_run_event_state(run_state);

//TODO: consider this case:
// new dump is created, post-create detects that it is a dup,
// but then FillCrashInfo(dup_name) *FAILS*.
// In this case, we later delete damaged dup_name (right?)
// but new dump never gets its FILENAME_COUNT set!

    /* Is crash a dup? (In this case, is_crash_a_dup() should have
     * aborted "post-create" event processing as soon as it saw uuid
     * and determined that there is another crash with same uuid.
     * In this case it sets state.crash_dump_dup_name)
     */
    if (!state.crash_dump_dup_name)
    {
        /* No. Was there error on one of processing steps in run_event? */
        if (r != 0)
            goto ret; /* yes */

        /* Was uuid created after all? (In this case, is_crash_a_dup()
         * should have fetched it and created state.uuid)
         */
        if (!state.uuid)
        {
            /* no */
            log("Dump directory '%s' has no UUID element", dump_dir_name);
            goto ret;
        }
    }
    else
    {
        dump_dir_name = state.crash_dump_dup_name;
    }

    /* Loads pCrashData (from the *first debugdump dir* if this one is a dup)
     * Returns:
     * MW_OCCURRED: "crash count is != 1" (iow: it is > 1 - dup)
     * MW_OK: "crash count is 1" (iow: this is a new crash, not a dup)
     * else: an error code
     */
    {
        dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
        if (!dd)
        {
            res = MW_ERROR;
            goto ret;
        }
        char *count_str = dd_load_text_ext(dd, FILENAME_COUNT, DD_FAIL_QUIETLY);
        unsigned long count = strtoul(count_str, NULL, 10);
        count++;
        char new_count_str[sizeof(long)*3 + 2];
        sprintf(new_count_str, "%lu", count);
        dd_save_text(dd, FILENAME_COUNT, new_count_str);
        dd_close(dd);

        res = FillCrashInfo(dump_dir_name, pCrashData);
        if (res == MW_OK)
        {
            if (count > 1)
            {
                log("Crash dump is a duplicate of %s", dump_dir_name);
                res = MW_OCCURRED;
            }
        }
    }

 ret:
    free(state.uuid);
    free(state.uid);
    free(state.crash_dump_dup_name);
    free(analyzer);

    return res;
}

static mw_result_t FillCrashInfo(const char *dump_dir_name,
                          map_crash_data_t& pCrashData)
{
    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        return MW_ERROR;

    load_crash_data_from_crash_dump_dir(dd, pCrashData);
    char *events = list_possible_events(dd, NULL, "");
    dd_close(dd);

    add_to_crash_data_ext(pCrashData, CD_EVENTS, CD_SYS, CD_ISNOTEDITABLE, events);
    free(events);

    add_to_crash_data_ext(pCrashData, CD_DUMPDIR, CD_SYS, CD_ISNOTEDITABLE, dump_dir_name);

    return MW_OK;
}

vector_map_crash_data_t GetCrashInfos(long caller_uid)
{
    vector_map_crash_data_t retval;
    log("Getting crash infos...");

    DIR *dir = opendir(DEBUG_DUMPS_DIR);
    if (dir != NULL)
    {
        try
        {
            struct dirent *dent;
            while ((dent = readdir(dir)) != NULL)
            {
                if (dot_or_dotdot(dent->d_name))
                    continue; /* skip "." and ".." */

                char *dump_dir_name = concat_path_file(DEBUG_DUMPS_DIR, dent->d_name);

                struct stat statbuf;
                if (stat(dump_dir_name, &statbuf) != 0
                 || !S_ISDIR(statbuf.st_mode)
                ) {
                    goto next; /* not a dir, skip */
                }

                /* Skip directories which are not for this uid */
                if (caller_uid != 0) /* not called by root? */
                {
                    char *uid;
                    char caller_uid_str[sizeof(long) * 3 + 2];

                    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
                    if (!dd)
                        goto next;

                    sprintf(caller_uid_str, "%ld", caller_uid);
                    uid = dd_load_text(dd, FILENAME_UID);
                    if (strcmp(uid, caller_uid_str) != 0)
                    {
                        char *inform_all = dd_load_text_ext(dd, FILENAME_INFORMALL, DD_FAIL_QUIETLY);
                        bool for_all = string_to_bool(inform_all);
                        free(inform_all);
                        if (!for_all)
                        {
                            dd_close(dd);
                            goto next;
                        }
                    }
                    dd_close(dd);
                }

                {
                    map_crash_data_t info;
                    mw_result_t res = FillCrashInfo(dump_dir_name, info);
                    switch (res)
                    {
                        case MW_OK:
                            retval.push_back(info);
                            break;
                        case MW_ERROR:
                            error_msg("Dump directory %s doesn't exist or misses crucial files, deleting", dump_dir_name);
                            delete_crash_dump_dir(dump_dir_name);
                            break;
                        default:
                            break;
                    }
                }
 next:
                free(dump_dir_name);
            }
        }
        catch (CABRTException& e)
        {
            error_msg("%s", e.what());
        }
        closedir(dir);
    }

    return retval;
}

/*
 * Called in two cases:
 * (1) by StartJob dbus call -> CreateReportThread(), in the thread
 * (2) by CreateReport dbus call
 * In the second case, it finishes quickly, because previous
 * StartJob dbus call already did all the processing, and we just retrieve
 * the result from dump directory, which is fast.
 */
void CreateReport(const char* crash_id, long caller_uid, int force, map_crash_data_t& crashReport)
{
    /* FIXME: starting from here, any shared data must be protected with a mutex. */
    mw_result_t res = CreateCrashReport(crash_id, caller_uid, force, crashReport);
    switch (res)
    {
        case MW_OK:
            VERB2 log_map_crash_data(crashReport, "crashReport");
            break;
        case MW_NOENT_ERROR:
            error_msg("Can't find crash with id '%s'", crash_id);
            break;
        case MW_PERM_ERROR:
            error_msg("Can't find crash with id '%s'", crash_id);
            break;
        case MW_PLUGIN_ERROR:
            error_msg("Particular analyzer plugin isn't loaded or there is an error within plugin(s)");
            break;
        default:
            error_msg("Corrupted crash with id %s, deleting", crash_id);
            DeleteDebugDump(crash_id, /*caller_uid:*/ 0);
            break;
    }
}

typedef struct thread_data_t {
    pthread_t thread_id;
    long caller_uid;
    int force;
    char* crash_id;
    char* peer;
} thread_data_t;
static void* create_report(void* arg)
{
    thread_data_t *thread_data = (thread_data_t *) arg;

    /* Client name is per-thread, need to set it */
    set_client_name(thread_data->peer);

    try
    {
        log("Creating report...");
        map_crash_data_t crashReport;
        CreateReport(thread_data->crash_id, thread_data->caller_uid, thread_data->force, crashReport);
        g_pCommLayer->JobDone(thread_data->peer);
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
    }
    catch (...) {}
    set_client_name(NULL);

    /* free strduped strings */
    free(thread_data->crash_id);
    free(thread_data->peer);
    free(thread_data);

    /* Bogus value. pthreads require us to return void* */
    return NULL;
}
int CreateReportThread(const char* crash_id, long caller_uid, int force, const char* pSender)
{
    thread_data_t *thread_data = (thread_data_t *)xzalloc(sizeof(thread_data_t));
    thread_data->crash_id = xstrdup(crash_id);
    thread_data->caller_uid = caller_uid;
    thread_data->force = force;
    thread_data->peer = xstrdup(pSender);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&thread_data->thread_id, &attr, create_report, thread_data);
    pthread_attr_destroy(&attr);
    if (r != 0)
    {
        free(thread_data->crash_id);
        free(thread_data->peer);
        free(thread_data);
        /* The only reason this may happen is system-wide resource starvation,
         * or ulimit is exceeded (someone floods us with CreateReport() dbus calls?)
         */
        error_msg("Can't create thread");
        return r;
    }
    VERB3 log("Thread %llx created", (unsigned long long)thread_data->thread_id);
    return r;
}


/* Remove dump dir */
int DeleteDebugDump(const char *dump_dir_name, long caller_uid)
{
    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        return MW_NOENT_ERROR;

    if (caller_uid != 0) /* not called by root */
    {
        char caller_uid_str[sizeof(long) * 3 + 2];
        sprintf(caller_uid_str, "%ld", caller_uid);

        char *uid = dd_load_text(dd, FILENAME_UID);
        if (strcmp(uid, caller_uid_str) != 0)
        {
            char *inform_all = dd_load_text_ext(dd, FILENAME_INFORMALL, DD_FAIL_QUIETLY);
            if (!string_to_bool(inform_all))
            {
                dd_close(dd);
                error_msg("crash '%s' can't be accessed by user with uid %ld", dump_dir_name, caller_uid);
                return 1;
            }
        }
    }

    dd_delete(dd);

    return 0; /* success */
}

void GetPluginsInfo(map_map_string_t &map_of_plugin_info)
{
    DIR *dir = opendir(PLUGINS_CONF_DIR);
    if (!dir)
        return;

    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL)
    {
        if (!is_regular_file(dent, PLUGINS_CONF_DIR))
            continue;
        char *ext = strrchr(dent->d_name, '.');
        if (!ext || strcmp(ext + 1, PLUGINS_CONF_EXTENSION) != 0)
            continue;
        VERB3 log("Found %s", dent->d_name);
        *ext = '\0';

        char *glade_file = xasprintf(PLUGINS_LIB_DIR"/%s.glade", dent->d_name);
        if (access(glade_file, F_OK) == 0)
        {
            *ext = '.';
            char *conf_file = concat_path_file(PLUGINS_CONF_DIR, dent->d_name);
            *ext = '\0';
            FILE *fp = fopen(conf_file, "r");
            free(conf_file);

            char *descr = NULL;
            if (fp)
            {
                descr = xmalloc_fgetline(fp);
                fclose(fp);
                if (descr && strncmp("# Description:", descr, strlen("# Description:")) == 0)
                    overlapping_strcpy(descr, skip_whitespace(descr + strlen("# Description:")));
                else
                {
                    free(descr);
                    descr = NULL;
                }
            }
            map_string_t plugin_info;
            plugin_info["Name"] = dent->d_name;
            plugin_info["Enabled"] = "yes";
            plugin_info["Type"] = "Reporter";       //was: plugin_type_str[module->GetType()]; field to be removed
            plugin_info["Version"] = VERSION;       //was: module->GetVersion(); field to be removed?
            plugin_info["Description"] = descr ? descr : ""; //was: module->GetDescription();
            plugin_info["Email"] = "";              //was: module->GetEmail(); field to be removed
            plugin_info["WWW"] = "";                //was: module->GetWWW(); field to be removed
            plugin_info["GTKBuilder"] = glade_file; //was: module->GetGTKBuilder();
            free(descr);
            map_of_plugin_info[dent->d_name] = plugin_info;
        }
        free(glade_file);

    }
    closedir(dir);
}

void GetPluginSettings(const char *plugin_name, map_plugin_settings_t &plugin_settings)
{
    char *conf_file = xasprintf(PLUGINS_CONF_DIR"/%s.conf", plugin_name);
    if (LoadPluginSettings(conf_file, plugin_settings, /*skip w/o value:*/ false))
        VERB3 log("Loaded %s.conf", plugin_name);
    free(conf_file);
}