#include "pch.h"
#include <nlohmann/json.hpp>
#include "QueueRoutes.h"
#include "../core/Plugin.h"
#include "../tasks/CAddSongsQueueTask.h"
#include "../tasks/CDeleteSongsQueueTask.h"
#include "../tasks/CMoveSongQueueTask.h"
#include "../tasks/CGetQueueTask.h"

using json = nlohmann::json;

static void HandleAddSongsToQueue(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    try
    {
        json body = json::parse(req.body);
        std::string playlistId = body.at("playlistId").get<std::string>();
        std::vector<int> songsIndexes = body.at("songsIndexes").get<std::vector<int>>();
        bool insertAtBeginning = body.at("insertAtBeginning").get<bool>();

        CAddSongsQueueTask* task = new CAddSongsQueueTask(plugin, playlistId, songsIndexes, insertAtBeginning);

        HRESULT hr = plugin->GetThreadService()->ExecuteInMainThread(task, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);

        if (SUCCEEDED(hr) && !task->HasErrors())
        {
            res.status = 204;
        }
        else
        {
            res.status = 500;
            res.set_content(json{ {"error", "Failed to add songs to queue"} }.dump(), "application/json");
        }

        task->Release();

    }
    catch (const json::exception)
    {
        res.status = 422;
        res.set_content(json{ {"error", "Invalid data in JSON"} }.dump(), "application/json");
    }
}

static void HandleDeleteSongsFromQueue(MyPlugin* plugin, const httplib::Request & req, httplib::Response & res)
{
    try
    {
        json body = json::parse(req.body);
        std::string playlistId = body.at("playlistId").get<std::string>();
        std::vector<int> songsIndexes = body.at("songsIndexes").get<std::vector<int>>();
        
        CDeleteSongsQueueTask* task = new CDeleteSongsQueueTask(plugin, playlistId, songsIndexes);

        HRESULT hr = plugin->GetThreadService()->ExecuteInMainThread(task, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);

        if (SUCCEEDED(hr) && !task->HasErrors())
        {
            res.status = 204;
        }
        else {
            res.status = 500;
            res.set_content(json{ {"error", "Failed to add songs to queue"} }.dump(), "application/json");
        }

        task->Release();
    }
    catch (const json::exception)
    {
        res.status = 422;
        res.set_content(json{ {"error", "Invalid data in JSON"} }.dump(), "application/json");
    }
}

static void HandleModeSongInQueue(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    try
    {
        json body = json::parse(req.body);
        int songIndex = body.at("songIndex").get<int>();
        int targetIndex = body.at("targetIndex").get<int>();

        CMoveSongQueueTask* task = new CMoveSongQueueTask(plugin, songIndex, targetIndex);

        HRESULT hr = plugin->GetThreadService()->ExecuteInMainThread(task, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);

        if (SUCCEEDED(hr))
        {
            if (!task->HasErrors())
            {
                res.status = 204;
            }
            else 
            {
                res.status = 422;
                res.set_content(json{ {"error", "One or more item indexes are out of bounds or invalid."} }.dump(), "application/json");
            }
        }
        else
        {
            res.status = 500;
            res.set_content(json{ {"error", "Failed to move song within the queue."} }.dump(), "application/json");
        }

        task->Release();
    }
    catch (const json::exception)
    {
        res.status = 422;
        res.set_content(json{ {"error", "Invalid data in JSON"} }.dump(), "application/json");
    }
}

static void HandleGetQueue(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    CGetQueueTask* task = new CGetQueueTask(plugin);

    HRESULT hr = plugin->GetThreadService()->ExecuteInMainThread(task, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);

    if (SUCCEEDED(hr) && !task->HasErrors())
    {
        // replace: invalid UTF-8 in tags must not throw out of the handler and leak the task
        res.status = 200;
        res.set_content(task->GetResults().dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
    }
    else
    {
        res.status = 500;
        res.set_content(json{ {"error", "Failed to get queue items."} }.dump(), "application/json");
    }

    task->Release();
}

void RegisterQueueRoutes(MyPlugin* plugin, const std::string& prefix)
{
    auto& svr = plugin->GetHttpServer();

    // GET ENDPOINTS
    svr.Get(prefix, [plugin](const httplib::Request& req, httplib::Response& res)
        {HandleGetQueue(plugin, req, res); });

    // POST ENDPOINTS
    svr.Post(prefix + "/songs", [plugin](const httplib::Request& req, httplib::Response& res)
        {HandleAddSongsToQueue(plugin, req, res); });

    svr.Post(prefix + "/songs/move", [plugin](const httplib::Request& req, httplib::Response& res)
        {HandleModeSongInQueue(plugin, req, res); });

	// DELETE ENDPOINTS
    svr.Delete(prefix + "/songs", [plugin](const httplib::Request& req, httplib::Response& res)
        {HandleDeleteSongsFromQueue(plugin, req, res); });

}