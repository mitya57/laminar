///
/// Copyright 2015-2022 Oliver Giles
///
/// This file is part of Laminar
///
/// Laminar is free software: you can redistribute it and/or modify
/// it under the terms of the GNU General Public License as published by
/// the Free Software Foundation, either version 3 of the License, or
/// (at your option) any later version.
///
/// Laminar is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with Laminar.  If not, see <http://www.gnu.org/licenses/>
///
#include "laminar.h"
#include "server.h"
#include "conf.h"
#include "log.h"
#include "http.h"
#include "rpc.h"

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <fstream>
#include <optional>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// FNM_EXTMATCH isn't supported under musl
#if !defined(FNM_EXTMATCH)
#define FNM_EXTMATCH 0
#endif

// rapidjson::Writer with a StringBuffer is used a lot in Laminar for
// preparing JSON messages to send to HTTP clients. A small wrapper
// class here reduces verbosity later for this common use case.
class Json : public rapidjson::Writer<rapidjson::StringBuffer> {
public:
    Json() : rapidjson::Writer<rapidjson::StringBuffer>(buf) { StartObject(); }
    template<typename T>
    Json& set(const char* key, T value) { String(key); Int64(value); return *this; }
    Json& startObject(const char* key) { String(key); StartObject(); return *this; }
    Json& startArray(const char* key) { String(key); StartArray(); return *this; }
    const char* str() { EndObject(); return buf.GetString(); }
private:
    rapidjson::StringBuffer buf;
};
template<> Json& Json::set(const char* key, double value) { String(key); Double(value); return *this; }
template<> Json& Json::set(const char* key, const char* value) { String(key); String(value); return *this; }
template<> Json& Json::set(const char* key, std::string value) { String(key); String(value.c_str()); return *this; }

// short syntax helpers for kj::Path
template<typename T>
inline kj::Path operator/(const kj::Path& p, const T& ext) {
    return p.append(ext);
}
template<typename T>
inline kj::Path operator/(const std::string& p, const T& ext) {
    return kj::Path{p}/ext;
}

typedef std::string str;

class temp_transaction {
private:
    pqxx::connection conn;
    pqxx::nontransaction tx;

public:
    temp_transaction(const char *connection_string) :
        conn(connection_string),
        tx(conn)
    { }

    pqxx::nontransaction *operator->() {
        return &tx;
    }

    pqxx::nontransaction &ref() {
        return tx;
    }
};

Laminar::Laminar(Server &server, Settings settings) :
    settings(settings),
    srv(server),
    homePath(kj::Path::parse(&settings.home[1])),
    fsHome(kj::newDiskFilesystem()->getRoot().openSubdir(homePath, kj::WriteMode::MODIFY)),
    http(kj::heap<Http>(*this)),
    rpc(kj::heap<Rpc>(*this))
{
    LASSERT(settings.home[0] == '/');

    if(fsHome->exists(homePath/"cfg"/"nodes")) {
        LLOG(ERROR, "Found node configuration directory cfg/nodes. Nodes have been deprecated, please migrate to contexts. Laminar will now exit.");
        exit(EXIT_FAILURE);
    }

    archiveUrl = settings.archive_url;
    if(archiveUrl.back() != '/')
        archiveUrl.append("/");

    numKeepRunDirs = 0;

    temp_transaction tx(settings.connection_string);

    // Prepare database for first use
    // TODO: error handling
    tx->exec("CREATE EXTENSION IF NOT EXISTS \"uuid-ossp\"");

    tx->exec(R"sql(
        CREATE TABLE IF NOT EXISTS builds
          ( guid        UUID   DEFAULT uuid_generate_v4() PRIMARY KEY
          , number      BIGINT NOT NULL
          , queuedAt    BIGINT NOT NULL
          , startedAt   BIGINT
          , completedAt BIGINT
          , result      INT
          , outputLen   BIGINT
          , parentBuild BIGINT
          , name        TEXT   NOT NULL
          , output      BYTEA
          , parentJob   TEXT
          , reason      TEXT
          )
    )sql");

    tx->exec(R"sql(
        CREATE TABLE IF NOT EXISTS artifacts
          ( guid        UUID   DEFAULT uuid_generate_v4() PRIMARY KEY
          , number      BIGINT NOT NULL
          , filesize    BIGINT NOT NULL
          , name        TEXT   NOT NULL
          , filename    TEXT   NOT NULL
          , CONSTRAINT fk_name_number FOREIGN KEY (name, number) REFERENCES builds(name, number)
          )
    )sql");

    tx->exec(R"sql(
        CREATE UNIQUE INDEX IF NOT EXISTS idx_name_number ON builds
          (name, number DESC)
    )sql");

    tx->exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_completion_time ON builds
          (completedAt DESC)
    )sql");

    tx->exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_completed ON builds
          (name)
        WHERE result IS NOT NULL
    )sql");

    tx->exec(R"sql(
        CREATE UNIQUE INDEX IF NOT EXISTS idx_name_number_filename ON artifacts
          (name, number, filename)
    )sql");

    tx->exec(R"sql(
        CREATE MATERIALIZED VIEW IF NOT EXISTS build_time_changes AS
        SELECT names.name
             , STRING_AGG(CAST(number AS TEXT), ',') AS numbers
             , STRING_AGG(CAST(diff AS TEXT), ',') AS durations
        FROM (SELECT DISTINCT name FROM builds) AS names
        JOIN LATERAL (SELECT builds.name, number, completedAt-startedAt AS diff
                      FROM builds WHERE builds.name = names.name
                      ORDER BY number DESC LIMIT 10
                     ) AS builds_last10 ON true
        GROUP BY names.name
        ORDER BY (MAX(diff)-MIN(diff))-STDDEV(diff) DESC
        LIMIT 8
    )sql");

    tx->exec(R"sql(
        CREATE MATERIALIZED VIEW IF NOT EXISTS builds_per_day AS
        SELECT result
             , CAST(EXTRACT('epoch' FROM NOW()) AS BIGINT)/86400 - completedAt/86400 AS day
             , COUNT(*) AS cnt
        FROM builds
        WHERE CAST(EXTRACT('epoch' FROM NOW()) AS BIGINT)/86400 - completedAt/86400 <= 6
        GROUP BY 1, 2
    )sql");

    tx->exec(R"sql(
        CREATE MATERIALIZED VIEW IF NOT EXISTS low_pass_rates AS
        SELECT name
             , CAST(COUNT(1) FILTER (WHERE result=5) AS FLOAT)/COUNT(*) AS pass_rate
        FROM builds
        GROUP BY name
        ORDER BY pass_rate ASC
        LIMIT 8
    )sql");

    tx->exec(R"sql(
        CREATE MATERIALIZED VIEW IF NOT EXISTS time_per_job AS
        SELECT name
             , AVG(completedAt-startedAt) AS av
        FROM builds
        WHERE completedAt > EXTRACT('epoch' FROM NOW()) - 7 * 86400
        GROUP BY name
        ORDER BY av DESC
        LIMIT 8
    )sql");

    tx->exec(R"sql(
        CREATE MATERIALIZED VIEW IF NOT EXISTS result_changed AS
        WITH stats AS (
            SELECT name
                 , MAX(number) FILTER (WHERE result = 5) AS last_success
                 , MAX(number) FILTER (WHERE result <> 5) AS last_failure
            FROM builds
            GROUP BY name
        )
        SELECT name, last_success, last_failure
        FROM stats
        WHERE last_success IS NOT NULL
        AND last_failure IS NOT NULL
        ORDER BY last_success - last_failure
        LIMIT 8
    )sql");

    tx->exec(R"sql(
        CREATE MATERIALIZED VIEW IF NOT EXISTS builds_per_job AS
        SELECT name
             , COUNT(*) AS c
        FROM builds
        WHERE completedAt > EXTRACT('epoch' FROM NOW()) - 86400
        GROUP BY name
        ORDER BY c DESC
        LIMIT 5
    )sql");

    // retrieve the last build numbers
    tx->exec("SELECT name, MAX(number) FROM builds GROUP BY name")
    .for_each([this](str name, uint build){
        buildNums[name] = build;
    });

    srv.watchPaths([this]{
        LLOG(INFO, "Reloading configuration");
        loadConfiguration();
        // config change may allow stuck jobs to dequeue
        assignNewJobs();
    }).addPath((homePath/"cfg"/"contexts").toString(true).cStr())
      .addPath((homePath/"cfg"/"jobs").toString(true).cStr())
      .addPath((homePath/"cfg").toString(true).cStr()); // for groups.conf

    loadCustomizations();
    srv.watchPaths([this]{
        LLOG(INFO, "Reloading customizations");
        loadCustomizations();
    }).addPath((homePath/"custom").toString(true).cStr());

    srv.listenRpc(*rpc, settings.bind_rpc);
    srv.listenHttp(*http, settings.bind_http);

    // Load configuration, may be called again in response to an inotify event
    // that the configuration files have been modified
    loadConfiguration();
}

void Laminar::loadCustomizations() {
    KJ_IF_MAYBE(templ, fsHome->tryOpenFile(kj::Path{"custom","index.html"})) {
        http->setHtmlTemplate((*templ)->readAllText().cStr());
    } else {
        http->setHtmlTemplate();
    }
}

uint Laminar::latestRun(std::string job) {
    if(auto it = buildNums.find(job); it != buildNums.end())
        return it->second;
    return 0;
}

bool Laminar::handleLogRequest(std::string name, uint num, std::string& output, bool& complete) {
    if(Run* run = activeRun(name, num)) {
        output = run->log;
        complete = false;
        return true;
    } else { // it must be finished, fetch it from the database
        temp_transaction tx(settings.connection_string);
        tx->exec_params("SELECT output FROM builds WHERE name = $1 AND number = $2",
                        name, num)
        .for_each([&](std::basic_string<std::byte> maybeZipped) {
            // TODO: Can we avoid a copy here?
            output = str(reinterpret_cast<const char*>(maybeZipped.c_str()));
        });
        if(output.size()) {
            complete = true;
            return true;
        }
    }
    return false;
}

bool Laminar::setParam(std::string job, uint buildNum, std::string param, std::string value) {
    if(Run* run = activeRun(job, buildNum)) {
        run->params[param] = value;
        return true;
    }
    return false;
}

const std::list<std::shared_ptr<Run>>& Laminar::listQueuedJobs() {
    return queuedJobs;
}

const RunSet& Laminar::listRunningJobs() {
    return activeJobs;
}

std::list<std::string> Laminar::listKnownJobs() {
    std::list<std::string> res;
    KJ_IF_MAYBE(dir, fsHome->tryOpenSubdir(kj::Path{"cfg","jobs"})) {
        for(kj::Directory::Entry& entry : (*dir)->listEntries()) {
            if(entry.name.endsWith(".run")) {
                res.emplace_back(entry.name.cStr(), entry.name.findLast('.').orDefault(0));
            }
        }
    }
    return res;
}

void Laminar::populateArtifacts(Json &j, std::string job, uint num, pqxx::stream_to *stream, kj::Path subdir) const {
    kj::Path runArchive{job,std::to_string(num)};
    runArchive = runArchive.append(subdir);
    KJ_IF_MAYBE(dir, fsHome->tryOpenSubdir("archive"/runArchive)) {
        for(kj::StringPtr file : (*dir)->listNames()) {
            kj::FsNode::Metadata meta = (*dir)->lstat(kj::Path{file});
            if(meta.type == kj::FsNode::Type::FILE) {
                j.StartObject();
                j.set("url", archiveUrl + (runArchive/file).toString().cStr());
                j.set("filename", (subdir/file).toString().cStr());
                j.set("size", meta.size);
                j.EndObject();
                if (stream != nullptr) {
                    std::tuple<str, uint, str, uint> row{job, num, (subdir/file).toString().cStr(), meta.size};
                    *stream << row;
                }
            } else if(meta.type == kj::FsNode::Type::DIRECTORY) {
                populateArtifacts(j, job, num, stream, subdir/file);
            }
        }
    }
}

void Laminar::populateArtifactsFromDB(Json &j, std::string job, uint num) const {
    kj::Path runArchive{job,std::to_string(num)};
    temp_transaction tx(settings.connection_string);
    tx->exec_params("SELECT filename, filesize FROM artifacts WHERE name = $1 AND number = $2",
                    job, num)
    .for_each([&](str fileName, uint fileSize) {
        j.StartObject();
        j.set("url", archiveUrl + (runArchive/fileName).toString().cStr());
        j.set("filename", fileName);
        j.set("size", fileSize);
        j.EndObject();
    });
}

std::string Laminar::getStatus(MonitorScope scope) {
    temp_transaction tx(settings.connection_string);
    Json j;
    j.set("type", "status");
    j.set("title", getenv("LAMINAR_TITLE") ?: "Laminar");
    j.set("version", laminar_version());
    j.set("time", time(nullptr));
    j.startObject("data");
    if(scope.type == MonitorScope::RUN) {
        bool isCompleted = false;
        tx->exec_params("SELECT queuedAt,startedAt,completedAt,result,reason,parentJob,parentBuild,q.lr FROM builds "
                        "LEFT JOIN (SELECT DISTINCT ON (name) name n, completedAt-startedAt lr FROM builds WHERE result IS NOT NULL ORDER BY name, number DESC) q ON q.n = name "
                        "WHERE name = $1 AND number = $2",
                        scope.job, scope.num)
        .for_each([&](time_t queued,
                      std::optional<time_t> started,
                      std::optional<time_t> completed,
                      std::optional<int> result,
                      std::optional<std::string> reason,
                      std::optional<std::string> parentJob,
                      uint parentBuild,
                      std::optional<uint> lastRuntime) {
            j.set("queued", queued);
            j.set("started", started.value_or(0));
            if(completed) {
              j.set("completed", *completed);
              isCompleted = true;
            }
            j.set("result", to_string(completed ? RunState(result.value_or(0)) : started ? RunState::RUNNING : RunState::QUEUED));
            j.set("reason", reason.value_or(""));
            j.startObject("upstream").set("name", parentJob.value_or("")).set("num", parentBuild).EndObject(2);
            if(lastRuntime)
              j.set("etc", started.value_or(0) + *lastRuntime);
        });
        if(auto it = buildNums.find(scope.job); it != buildNums.end())
            j.set("latestNum", int(it->second));

        j.startArray("artifacts");
        if (isCompleted)
            populateArtifactsFromDB(j, scope.job, scope.num);
        else
            populateArtifacts(j, scope.job, scope.num);
        j.EndArray();
    } else if(scope.type == MonitorScope::JOB) {
        const uint runsPerPage = 20;
        j.startArray("recent");
        // ORDER BY param cannot be bound
        std::string order_by;
        std::string direction = scope.order_desc ? "DESC" : "ASC";
        if(scope.field == "number")
            order_by = "number " + direction;
        else if(scope.field == "result")
            order_by = "result " + direction + ", number DESC";
        else if(scope.field == "started")
            order_by = "startedAt " + direction + ", number DESC";
        else if(scope.field == "duration")
            order_by = "(completedAt-startedAt) " + direction + ", number DESC";
        else
            order_by = "number DESC";
        std::string stmt = "SELECT number,startedAt,completedAt,result,reason FROM builds "
                "WHERE name = $1 AND result IS NOT NULL ORDER BY "
                + order_by + " LIMIT $2 OFFSET $3";
        tx->exec_params(stmt, scope.job, runsPerPage, scope.page * runsPerPage)
        .for_each([&](uint build,time_t started,time_t completed,int result,std::optional<str> reason){
            j.StartObject();
            j.set("number", build)
             .set("completed", completed)
             .set("started", started)
             .set("result", to_string(RunState(result)))
             .set("reason", reason.value_or(""))
             .EndObject();
        });
        j.EndArray();
        tx->exec_params("SELECT COUNT(*),CAST(AVG(completedAt-startedAt) AS INT) FROM builds WHERE name = $1 AND result IS NOT NULL",
                        scope.job)
        .for_each([&](uint nRuns, std::optional<uint> averageRuntime){
            j.set("averageRuntime", averageRuntime.value_or(0));
            j.set("pages", (nRuns-1) / runsPerPage + 1);
            j.startObject("sort");
            j.set("page", scope.page)
             .set("field", scope.field)
             .set("order", scope.order_desc ? "dsc" : "asc")
             .EndObject();
        });
        j.startArray("running");
        auto p = activeJobs.byJobName().equal_range(scope.job);
        for(auto it = p.first; it != p.second; ++it) {
            const std::shared_ptr<Run> run = *it;
            j.StartObject();
            j.set("number", run->build);
            j.set("context", run->context->name);
            j.set("started", run->startedAt);
            j.set("result", to_string(RunState::RUNNING));
            j.set("reason", run->reason());
            j.EndObject();
        }
        j.EndArray();
        j.startArray("queued");
        for(const auto& run : queuedJobs) {
            if (run->name == scope.job) {
                j.StartObject();
                j.set("number", run->build);
                j.set("result", to_string(RunState::QUEUED));
                j.set("reason", run->reason());
                j.EndObject();
            }
        }
        j.EndArray();
        tx->exec_params("SELECT number,startedAt FROM builds WHERE name = $1 AND result = $2 "
                        "ORDER BY completedAt DESC LIMIT 1",
                        scope.job, int(RunState::SUCCESS))
        .for_each([&](int build, time_t started){
            j.startObject("lastSuccess");
            j.set("number", build).set("started", started);
            j.EndObject();
        });
        tx->exec_params("SELECT number,startedAt FROM builds "
                        "WHERE name = $1 AND result <> $2 "
                        "ORDER BY completedAt DESC LIMIT 1",
                        scope.job, int(RunState::SUCCESS))
        .for_each([&](int build, time_t started){
            j.startObject("lastFailed");
            j.set("number", build).set("started", started);
            j.EndObject();
        });
        auto desc = jobDescriptions.find(scope.job);
        j.set("description", desc == jobDescriptions.end() ? "" : desc->second);
    } else if(scope.type == MonitorScope::ALL) {
        j.startArray("jobs");
        tx->exec("SELECT DISTINCT ON (name) name, number, startedAt, completedAt, result, reason "
                 "FROM builds ORDER BY name, number DESC")
        .for_each([&](str name,uint number, std::optional<time_t> started, std::optional<time_t> completed, std::optional<int> result, std::optional<str> reason){
            j.StartObject();
            j.set("name", name);
            j.set("number", number);
            j.set("result", to_string(RunState(result.value_or(0))));
            j.set("started", started.value_or(0));
            j.set("completed", completed.value_or(0));
            j.set("reason", reason.value_or(""));
            j.EndObject();
        });
        j.EndArray();
        j.startArray("running");
        for(const auto& run : activeJobs.byStartedAt()) {
            j.StartObject();
            j.set("name", run->name);
            j.set("number", run->build);
            j.set("context", run->context->name);
            j.set("started", run->startedAt);
            j.EndObject();
        }
        j.EndArray();
        j.startObject("groups");
        for(const auto& group : jobGroups)
            j.set(group.first.c_str(), group.second);
        j.EndObject();
    } else { // Home page
        j.startArray("recent");
        tx->exec("SELECT name,number,node,queuedAt,startedAt,completedAt,result,reason FROM builds WHERE completedAt IS NOT NULL ORDER BY completedAt DESC LIMIT 20")
        .for_each([&](str name,uint build,std::optional<str> context,time_t queued,time_t started,time_t completed,int result,std::optional<str> reason){
            j.StartObject();
            j.set("name", name)
             .set("number", build)
             .set("context", context.value_or(""))
             .set("queued", queued)
             .set("started", started)
             .set("completed", completed)
             .set("result", to_string(RunState(result)))
             .set("reason", reason.value_or(""))
             .EndObject();
        });
        j.EndArray();
        j.startArray("running");
        for(const auto& run : activeJobs.byStartedAt()) {
            j.StartObject();
            j.set("name", run->name);
            j.set("number", run->build);
            j.set("context", run->context->name);
            j.set("started", run->startedAt);
            tx->exec_params("SELECT completedAt - startedAt FROM builds "
                            "WHERE completedAt IS NOT NULL AND name = $1 "
                            "ORDER BY completedAt DESC LIMIT 1",
                            run->name)
            .for_each([&](uint lastRuntime){
                j.set("etc", run->startedAt + lastRuntime);
            });
            j.EndObject();
        }
        j.EndArray();
        j.startArray("queued");
        for(const auto& run : queuedJobs) {
            j.StartObject();
            j.set("name", run->name);
            j.set("number", run->build);
            j.set("result", to_string(RunState::QUEUED));
            j.EndObject();
        }
        j.EndArray();
        int execTotal = 0;
        int execBusy = 0;
        for(const auto& it : contexts) {
            const std::shared_ptr<Context>& context = it.second;
            execTotal += context->numExecutors;
            execBusy += context->busyExecutors;
        }
        j.set("executorsTotal", execTotal);
        j.set("executorsBusy", execBusy);
        j.startArray("buildsPerDay");
        for(int i = 6; i >= 0; --i) {
            j.StartObject();
            tx->exec_params("SELECT result, cnt FROM builds_per_day WHERE day = $1", i)
            .for_each([&](int result, int num){
                j.set(to_string(RunState(result)).c_str(), num);
            });
            j.EndObject();
        }
        j.EndArray();
        j.startObject("buildsPerJob");
        tx->exec("SELECT name, c FROM builds_per_job")
        .for_each([&](str job, int count){
            j.set(job.c_str(), count);
        });
        j.EndObject();
        j.startObject("timePerJob");
        tx->exec("SELECT name, av FROM time_per_job")
        .for_each([&](str job, double time){
            j.set(job.c_str(), time);
        });
        j.EndObject();
        j.startArray("resultChanged");
        tx->exec("SELECT name, last_success, last_failure FROM result_changed")
        .for_each([&](str job, uint lastSuccess, uint lastFailure){
            j.StartObject();
            j.set("name", job)
             .set("lastSuccess", lastSuccess)
             .set("lastFailure", lastFailure);
            j.EndObject();
        });
        j.EndArray();
        j.startArray("lowPassRates");
        tx->exec("SELECT name, pass_rate FROM low_pass_rates")
        .for_each([&](str job, double passRate){
            j.StartObject();
            j.set("name", job).set("passRate", passRate);
            j.EndObject();
        });
        j.EndArray();
        j.startArray("buildTimeChanges");
        tx->exec("SELECT name, numbers, durations FROM build_time_changes")
        .for_each([&](str name, str numbers, std::optional<str> durations){
            j.StartObject();
            j.set("name", name);
            j.startArray("numbers");
            j.RawValue(numbers.data(), numbers.length(), rapidjson::Type::kArrayType);
            j.EndArray();
            j.startArray("durations");
            j.RawValue(durations.value_or("").data(), durations.value_or("").length(), rapidjson::Type::kArrayType);
            j.EndArray();
            j.EndObject();
        });
        j.EndArray();
        j.startObject("completedCounts");
        tx->exec("SELECT name, COUNT(*) FROM builds WHERE result IS NOT NULL GROUP BY name")
        .for_each([&](str job, uint count){
            j.set(job.c_str(), count);
        });
        j.EndObject();
    }
    j.EndObject();
    return j.str();
}

Laminar::~Laminar() noexcept { }

bool Laminar::loadConfiguration() {
    if(const char* ndirs = getenv("LAMINAR_KEEP_RUNDIRS"))
        numKeepRunDirs = static_cast<uint>(atoi(ndirs));

    std::set<std::string> knownContexts;

    KJ_IF_MAYBE(contextsDir, fsHome->tryOpenSubdir(kj::Path{"cfg","contexts"})) {
        for(kj::Directory::Entry& entry : (*contextsDir)->listEntries()) {
            if(!entry.name.endsWith(".conf"))
                continue;

            StringMap conf = parseConfFile((homePath/"cfg"/"contexts"/entry.name).toString(true).cStr());

            std::string name(entry.name.cStr(), entry.name.findLast('.').orDefault(0));
            auto existing = contexts.find(name);
            std::shared_ptr<Context> context = existing == contexts.end() ? contexts.emplace(name, std::shared_ptr<Context>(new Context)).first->second : existing->second;
            context->name = name;
            context->numExecutors = conf.get<int>("EXECUTORS", 6);

            std::string jobPtns = conf.get<std::string>("JOBS");
            std::set<std::string> jobPtnsList;
            if(!jobPtns.empty()) {
                std::istringstream iss(jobPtns);
                std::string job;
                while(std::getline(iss, job, ','))
                    jobPtnsList.insert(job);
            }
            context->jobPatterns.swap(jobPtnsList);

            knownContexts.insert(name);
        }
    }

    // remove any contexts whose config files disappeared.
    // if there are no known contexts, take care not to remove and re-add the default context.
    for(auto it = contexts.begin(); it != contexts.end();) {
        if((it->first == "default" && knownContexts.size() == 0) || knownContexts.find(it->first) != knownContexts.end())
            it++;
        else
            it = contexts.erase(it);
    }

    // add a default context
    if(contexts.empty()) {
        LLOG(INFO, "Creating a default context with 6 executors");
        std::shared_ptr<Context> context(new Context);
        context->name = "default";
        context->numExecutors = 6;
        contexts.emplace("default", context);
    }

    KJ_IF_MAYBE(jobsDir, fsHome->tryOpenSubdir(kj::Path{"cfg","jobs"})) {
        for(kj::Directory::Entry& entry : (*jobsDir)->listEntries()) {
            if(!entry.name.endsWith(".conf"))
                continue;
            StringMap conf = parseConfFile((homePath/"cfg"/"jobs"/entry.name).toString(true).cStr());

            std::string jobName(entry.name.cStr(), entry.name.findLast('.').orDefault(0));

            std::string ctxPtns = conf.get<std::string>("CONTEXTS");

            std::set<std::string> ctxPtnList;
            if(!ctxPtns.empty()) {
                std::istringstream iss(ctxPtns);
                std::string ctx;
                while(std::getline(iss, ctx, ','))
                    ctxPtnList.insert(ctx);
            }
            // Must be present both here and in queueJob because otherwise if a context
            // were created while a job is already queued, the default context would be
            // dropped when the set of contexts is updated here.
            if(ctxPtnList.empty())
                ctxPtnList.insert("default");
            jobContexts[jobName].swap(ctxPtnList);

            std::string desc = conf.get<std::string>("DESCRIPTION");
            if(!desc.empty()) {
                jobDescriptions[jobName] = desc;
            }
        }
    }

    jobGroups.clear();
    KJ_IF_MAYBE(groupsConf, fsHome->tryOpenFile(kj::Path{"cfg","groups.conf"}))
        jobGroups = parseConfFile((homePath/"cfg"/"groups.conf").toString(true).cStr());
    if(jobGroups.empty())
        jobGroups["All Jobs"] = ".*";

    return true;
}

std::shared_ptr<Run> Laminar::queueJob(std::string name, ParamMap params, bool frontOfQueue) {
    if(!fsHome->exists(kj::Path{"cfg","jobs",name+".run"})) {
        LLOG(ERROR, "Non-existent job", name);
        return nullptr;
    }

    // jobContexts[name] can be empty if there is no .conf file at all
    if(jobContexts[name].empty())
        jobContexts.at(name).insert("default");

    std::shared_ptr<Run> run = std::make_shared<Run>(name, ++buildNums[name], kj::mv(params), homePath.clone());
    if(frontOfQueue)
        queuedJobs.push_front(run);
    else
        queuedJobs.push_back(run);

    temp_transaction tx(settings.connection_string);
    tx->exec_params("INSERT INTO builds(name,number,queuedAt,parentJob,parentBuild,reason) VALUES($1,$2,$3,$4,$5,$6)",
                    run->name, run->build, run->queuedAt, run->parentName, run->parentBuild, run->reason());

    // notify clients
    Json j;
    j.set("type", "job_queued")
        .startObject("data")
        .set("name", name)
        .set("number", run->build)
        .set("result", to_string(RunState::QUEUED))
        .set("queueIndex", frontOfQueue ? 0 : (queuedJobs.size() - 1))
        .set("reason", run->reason())
        .EndObject();
    http->notifyEvent(j.str(), name.c_str());

    assignNewJobs();
    return run;
}

bool Laminar::abort(std::string job, uint buildNum) {
    if(Run* run = activeRun(job, buildNum))
        return run->abort();
    return false;
}

void Laminar::abortAll() {
    for(std::shared_ptr<Run> run : activeJobs) {
        run->abort();
    }
}

bool Laminar::canQueue(const Context& ctx, const Run& run) const {
    if(ctx.busyExecutors >= ctx.numExecutors)
        return false;

    // match may be jobs as defined by the context...
    for(std::string p : ctx.jobPatterns) {
        if(fnmatch(p.c_str(), run.name.c_str(), FNM_EXTMATCH) == 0)
            return true;
    }

    // ...or context as defined by the job.
    for(std::string p : jobContexts.at(run.name)) {
        if(fnmatch(p.c_str(), ctx.name.c_str(), FNM_EXTMATCH) == 0)
            return true;
    }

    return false;
}

bool Laminar::tryStartRun(std::shared_ptr<Run> run, int queueIndex) {
    for(auto& sc : contexts) {
        std::shared_ptr<Context> ctx = sc.second;

        if(canQueue(*ctx, *run)) {
            RunState lastResult = RunState::UNKNOWN;

            // set the last known result if exists. Runs which haven't started yet should
            // have completedAt == NULL and thus be at the end of a DESC ordered query
            temp_transaction tx(settings.connection_string);
            tx->exec_params("SELECT result FROM builds WHERE name = $1 ORDER BY completedAt DESC LIMIT 1",
                            run->name)
            .for_each([&](std::optional<int> result){
                lastResult = RunState(result.value_or(0));
            });

            kj::Promise<RunState> onRunFinished = run->start(lastResult, ctx, *fsHome,[this](kj::Maybe<pid_t>& pid){return srv.onChildExit(pid);});

            tx->exec_params("UPDATE builds SET node = $1, startedAt = $2 WHERE name = $3 AND number = $4",
                            ctx->name, run->startedAt, run->name, run->build);

            ctx->busyExecutors++;

            kj::Promise<void> exec = srv.readDescriptor(run->output_fd, [this, run](const char*b, size_t n){
                // handle log output
                std::string s(b, n);
                run->log += s;
                http->notifyLog(run->name, run->build, s, false);
            }).then([run, p = kj::mv(onRunFinished)]() mutable {
                // wait until leader reaped
                return kj::mv(p);
            }).then([this, run](RunState){
                handleRunFinished(run.get());
            });
            if(run->timeout > 0) {
                exec = exec.attach(srv.addTimeout(run->timeout, [r=run.get()](){
                    r->abort();
                }));
            }
            srv.addTask(kj::mv(exec));
            LLOG(INFO, "Started job", run->name, run->build, ctx->name);

            // notify clients
            Json j;
            j.set("type", "job_started")
             .startObject("data")
             .set("queueIndex", queueIndex)
             .set("name", run->name)
             .set("queued", run->queuedAt)
             .set("started", run->startedAt)
             .set("number", run->build)
             .set("reason", run->reason());
            tx->exec_params("SELECT completedAt - startedAt FROM builds WHERE name = $1 ORDER BY completedAt DESC LIMIT 1",
                            run->name)
            .for_each([&](std::optional<uint> etc){
                j.set("etc", time(nullptr) + etc.value_or(0));
            });
            j.EndObject();
            http->notifyEvent(j.str(), run->name.c_str());
            return true;
        }
    }
    return false;
}

void Laminar::assignNewJobs() {
    auto it = queuedJobs.begin();
    while(it != queuedJobs.end()) {
        if(tryStartRun(*it, std::distance(it, queuedJobs.begin()))) {
            activeJobs.insert(*it);
            it = queuedJobs.erase(it);
        } else {
            ++it;
        }
    }
}

void Laminar::handleRunFinished(Run * r) {
    std::shared_ptr<Context> ctx = r->context;

    ctx->busyExecutors--;
    LLOG(INFO, "Run completed", r->name, to_string(r->result));
    time_t completedAt = time(nullptr);

    temp_transaction tx(settings.connection_string);
    tx->exec_params("UPDATE builds SET completedAt = $1, result = $2, output = $3, outputLen = $4 WHERE name = $5 AND number = $6",
                    completedAt, int(r->result), pqxx::binary_cast(r->log), r->log.length(), r->name, r->build);
    tx->exec("REFRESH MATERIALIZED VIEW build_time_changes");
    tx->exec("REFRESH MATERIALIZED VIEW builds_per_day");
    tx->exec("REFRESH MATERIALIZED VIEW low_pass_rates");
    tx->exec("REFRESH MATERIALIZED VIEW time_per_job");
    tx->exec("REFRESH MATERIALIZED VIEW result_changed");
    tx->exec("REFRESH MATERIALIZED VIEW builds_per_job");

    // notify clients
    Json j;
    j.set("type", "job_completed")
            .startObject("data")
            .set("name", r->name)
            .set("number", r->build)
            .set("queued", r->queuedAt)
            .set("completed", completedAt)
            .set("started", r->startedAt)
            .set("result", to_string(r->result))
            .set("reason", r->reason());
    j.startArray("artifacts");
    auto stream = pqxx::stream_to::table(tx.ref(), {"artifacts"}, {"name", "number", "filename", "filesize"});
    populateArtifacts(j, r->name, r->build, &stream);
    stream.complete();
    j.EndArray();
    j.EndObject();
    http->notifyEvent(j.str(), r->name);
    http->notifyLog(r->name, r->build, "", true);
    // erase reference to run from activeJobs. Since runFinished is called in a
    // lambda whose context contains a shared_ptr<Run>, the run won't be deleted
    // until the context is destroyed at the end of the lambda execution.
    activeJobs.byRunPtr().erase(r);

    // remove old run directories
    // We cannot count back the number of directories to keep from the currently
    // finishing job because there may well be older, still-running instances of
    // this job and we don't want to delete their rundirs. So instead, check
    // whether there are any more active runs of this job, and if so, count back
    // from the oldest among them. If there are none, count back from the latest
    // known build number of this job, which may not be that of the run that
    // finished here.
    auto it = activeJobs.byJobName().equal_range(r->name);
    uint oldestActive = (it.first == it.second)? buildNums[r->name] : (*it.first)->build - 1;
    for(int i = static_cast<int>(oldestActive - numKeepRunDirs); i > 0; i--) {
        kj::Path d{"run",r->name,std::to_string(i)};
        // Once the directory does not exist, it's probably not worth checking
        // any further. 99% of the time this loop should only ever have 1 iteration
        // anyway so hence this (admittedly debatable) optimization.
        if(!fsHome->exists(d))
            break;
        // must use a try/catch because remove will throw if deletion fails. Using
        // tryRemove does not help because it still throws an exception for some
        // errors such as EACCES
        try {
            fsHome->remove(d);
        } catch(kj::Exception& e) {
            LLOG(ERROR, "Could not remove directory", e.getDescription());
        }
    }

    fsHome->symlink(kj::Path{"archive", r->name, "latest"}, std::to_string(r->build), kj::WriteMode::CREATE|kj::WriteMode::MODIFY);

    // in case we freed up an executor, check the queue
    assignNewJobs();
}

kj::Maybe<kj::Own<const kj::ReadableFile>> Laminar::getArtefact(std::string path) {
    return fsHome->openFile(kj::Path("archive").append(kj::Path::parse(path)));
}

bool Laminar::handleBadgeRequest(std::string job, std::string &badge) {
    RunState rs = RunState::UNKNOWN;
    temp_transaction tx(settings.connection_string);
    tx->exec_params("SELECT result FROM builds WHERE name = $1 AND result IS NOT NULL ORDER BY number DESC LIMIT 1",
                    job)
    .for_each([&](int result){
        rs = RunState(result);
    });
    if(rs == RunState::UNKNOWN)
        return false;

    std::string status = to_string(rs);
    // Empirical approximation of pixel width. Not particularly stable.
    const int jobNameWidth = job.size() * 7 + 10;
    const int statusWidth = status.size() * 7 + 10;
    const char* gradient1 = (rs == RunState::SUCCESS) ? "#2aff4d" : "#ff2a2a";
    const char* gradient2 = (rs == RunState::SUCCESS) ? "#24b43c" : "#b42424";
    char* svg = NULL;
    if(asprintf(&svg,
R"x(
<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="20">
  <clipPath id="clip">
    <rect width="%d" height="20" rx="4"/>
  </clipPath>
  <linearGradient id="job" x1="0" x2="0" y1="0" y2="1">
    <stop offset="0" stop-color="#666" />
    <stop offset="1" stop-color="#333" />
  </linearGradient>
  <linearGradient id="status" x1="0" x2="0" y1="0" y2="1">
    <stop offset="0" stop-color="%s" />
    <stop offset="1" stop-color="%s" />
  </linearGradient>
  <g clip-path="url(#clip)" font-family="DejaVu Sans,Verdana,sans-serif" font-size="12" text-anchor="middle">
    <rect width="%d" height="20" fill="url(#job)"/>
    <text x="%d" y="14" fill="#fff">%s</text>
    <rect x="%d" width="%d" height="20" fill="url(#status)"/>
    <text x="%d" y="14" fill="#000">%s</text>
  </g>
</svg>)x", jobNameWidth+statusWidth, jobNameWidth+statusWidth, gradient1, gradient2, jobNameWidth, jobNameWidth/2+1, job.data(), jobNameWidth, statusWidth, jobNameWidth+statusWidth/2, status.data()) < 0)
        return false;

    badge = svg;
    return true;
}

