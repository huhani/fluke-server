#include "pch.h"
#include "PocRoutes.h"
#include "../core/Plugin.h"
#include "../core/PlaybackQueueExtension.h"
#include "../tasks/CMainThreadTask.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
    // Player track switching settings (IAIMPServicePlayer property list), exposed by name
    const std::pair<const char*, int> SWITCHING_PROPS[] = {
        {"stopAfterTrack", AIMP_PLAYER_PROPID_STOP_AFTER_TRACK},
        {"autoJumpToNextTrack", AIMP_PLAYER_PROPID_AUTO_JUMP_TO_NEXT_TRACK},
        {"autoSwitching", AIMP_PLAYER_PROPID_AUTOSWITCHING},
        {"autoCrossfadeMs", AIMP_PLAYER_PROPID_AUTOSWITCHING_CROSSFADE},
        {"autoFadeInMs", AIMP_PLAYER_PROPID_AUTOSWITCHING_FADEIN},
        {"autoFadeOutMs", AIMP_PLAYER_PROPID_AUTOSWITCHING_FADEOUT},
        {"autoPauseMs", AIMP_PLAYER_PROPID_AUTOSWITCHING_PAUSE_BETWEEN_TRACKS},
        {"manualSwitching", AIMP_PLAYER_PROPID_MANUALSWITCHING},
        {"manualCrossfadeMs", AIMP_PLAYER_PROPID_MANUALSWITCHING_CROSSFADE},
        {"manualFadeInMs", AIMP_PLAYER_PROPID_MANUALSWITCHING_FADEIN},
        {"manualFadeOutMs", AIMP_PLAYER_PROPID_MANUALSWITCHING_FADEOUT},
    };

    void SendJson(httplib::Response& res, int status, const json& body)
    {
        res.status = status;
        res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
    }

    // Must run on the main thread. A property that cannot be read is reported as null.
    json ReadSwitching(MyPlugin* plugin)
    {
        json values = json::object();
        IAIMPPropertyList* props = nullptr;
        if (FAILED(plugin->GetPlayerService()->QueryInterface(IID_IAIMPPropertyList, (void**)&props)))
            return values;

        for (const auto& [key, propId] : SWITCHING_PROPS)
        {
            int value = 0;
            if (SUCCEEDED(props->GetValueAsInt32(propId, &value)))
                values[key] = value;
            else
                values[key] = nullptr;
        }
        props->Release();
        return values;
    }
}

// =============================================================================
// Route Handlers
// =============================================================================

static void HandleGetQueueState(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    SendJson(res, 200, plugin->GetPlaybackQueue()->GetState());
}

static void HandleSetExtension(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    bool enabled = false;
    try
    {
        enabled = json::parse(req.body).at("enabled").get<bool>();
    }
    catch (const json::exception&)
    {
        SendJson(res, 422, {{"error", "Body must be {\"enabled\": bool}"}});
        return;
    }

    HRESULT hr = E_FAIL;
    HRESULT exec = RunInMainThread(plugin, [&] { hr = plugin->GetPlaybackQueue()->SetRegistered(enabled); });
    if (FAILED(exec) || FAILED(hr))
    {
        SendJson(res, 500, {{"error", "Failed to change extension registration"}, {"hr", HrText(FAILED(exec) ? exec : hr)}});
        return;
    }
    SendJson(res, 200, {{"registered", plugin->GetPlaybackQueue()->GetState()["registered"]}});
}

static void HandlePrepareNext(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    NextTrackRequest request;
    bool notify = true;
    try
    {
        json body = json::parse(req.body);
        request.path = body.at("path").get<std::string>();
        request.offsetMs = body.value("offsetMs", 0);
        request.crossfadeMs = body.value("crossfadeMs", -1);
        request.fadeInMs = body.value("fadeInMs", -1);
        request.fadeOutMs = body.value("fadeOutMs", -1);
        request.delayMs = body.value("delayMs", 0);
        notify = body.value("notify", true);
    }
    catch (const json::exception&)
    {
        SendJson(res, 422, {{"error", "Body must contain \"path\" (string); optional ints offsetMs, crossfadeMs, fadeInMs, fadeOutMs, delayMs and bool notify"}});
        return;
    }

    CPlaybackQueueExtension* queue = plugin->GetPlaybackQueue();
    HRESULT hr = E_FAIL;
    std::string error;
    HRESULT exec = RunInMainThread(plugin, [&] {
        hr = queue->PrepareSlot(request, error);
        if (SUCCEEDED(hr) && notify)
            queue->NotifyChanged();
    });

    if (FAILED(exec))
    {
        SendJson(res, 500, {{"error", "Main thread call failed"}, {"hr", HrText(exec)}});
        return;
    }
    if (FAILED(hr))
    {
        SendJson(res, hr == E_INVALIDARG ? 422 : 500, {{"error", error}, {"hr", HrText(hr)}});
        return;
    }
    SendJson(res, 200, {{"slot", queue->GetState()["slot"]}});
}

static void HandleClearNext(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    bool notify = true;
    if (!req.body.empty())
    {
        try
        {
            notify = json::parse(req.body).value("notify", true);
        }
        catch (const json::exception&)
        {
            SendJson(res, 422, {{"error", "Body must be empty or {\"notify\": bool}"}});
            return;
        }
    }

    CPlaybackQueueExtension* queue = plugin->GetPlaybackQueue();
    HRESULT exec = RunInMainThread(plugin, [&] {
        queue->ClearSlot();
        if (notify)
            queue->NotifyChanged();
    });
    if (FAILED(exec))
    {
        SendJson(res, 500, {{"error", "Main thread call failed"}, {"hr", HrText(exec)}});
        return;
    }
    res.status = 204;
}

static void HandleGetSwitching(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    json values;
    HRESULT exec = RunInMainThread(plugin, [&] { values = ReadSwitching(plugin); });
    if (FAILED(exec))
    {
        SendJson(res, 500, {{"error", "Main thread call failed"}, {"hr", HrText(exec)}});
        return;
    }
    SendJson(res, 200, values);
}

static void HandleSetSwitching(MyPlugin* plugin, const httplib::Request& req, httplib::Response& res)
{
    json body;
    try
    {
        body = json::parse(req.body);
    }
    catch (const json::exception&)
    {
        body = nullptr;
    }
    if (!body.is_object())
    {
        SendJson(res, 422, {{"error", "Body must be an object of switching settings"}});
        return;
    }

    json errors = json::object();
    json values;
    HRESULT exec = RunInMainThread(plugin, [&] {
        IAIMPPropertyList* props = nullptr;
        HRESULT hr = plugin->GetPlayerService()->QueryInterface(IID_IAIMPPropertyList, (void**)&props);
        if (FAILED(hr))
        {
            errors["propertyList"] = HrText(hr);
            return;
        }
        for (const auto& [key, propId] : SWITCHING_PROPS)
        {
            if (!body.contains(key))
                continue;
            const json& value = body[key];
            if (!value.is_boolean() && !value.is_number_integer())
            {
                errors[key] = "must be an integer or bool";
                continue;
            }
            int number = value.is_boolean() ? (value.get<bool>() ? 1 : 0) : value.get<int>();
            hr = props->SetValueAsInt32(propId, number);
            if (FAILED(hr))
                errors[key] = HrText(hr);
        }
        props->Release();
        values = ReadSwitching(plugin);
    });

    if (FAILED(exec))
    {
        SendJson(res, 500, {{"error", "Main thread call failed"}, {"hr", HrText(exec)}});
        return;
    }
    SendJson(res, errors.empty() ? 200 : 422, {{"values", values}, {"errors", errors}});
}

// =============================================================================
// Route Registration
// =============================================================================

void RegisterPocRoutes(MyPlugin* plugin, const std::string& prefix)
{
    auto& svr = plugin->GetHttpServer();

    svr.Get(prefix + "/queue", [plugin](const httplib::Request& req, httplib::Response& res)
        { HandleGetQueueState(plugin, req, res); });
    svr.Post(prefix + "/queue/extension", [plugin](const httplib::Request& req, httplib::Response& res)
        { HandleSetExtension(plugin, req, res); });
    svr.Post(prefix + "/queue/next", [plugin](const httplib::Request& req, httplib::Response& res)
        { HandlePrepareNext(plugin, req, res); });
    svr.Delete(prefix + "/queue/next", [plugin](const httplib::Request& req, httplib::Response& res)
        { HandleClearNext(plugin, req, res); });
    svr.Get(prefix + "/switching", [plugin](const httplib::Request& req, httplib::Response& res)
        { HandleGetSwitching(plugin, req, res); });
    svr.Post(prefix + "/switching", [plugin](const httplib::Request& req, httplib::Response& res)
        { HandleSetSwitching(plugin, req, res); });
}
