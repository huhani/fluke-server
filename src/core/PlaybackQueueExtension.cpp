#include "pch.h"
#include "PlaybackQueueExtension.h"

#include <chrono>

namespace
{
    const char* const MANAGED_PLAYLIST_NAME = "musicbank";
    const size_t LOG_CAPACITY = 100;

    long long NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::wstring ToWide(const std::string& utf8)
    {
        if (utf8.empty())
            return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], size);
        return wide;
    }

    // Windows paths are case-insensitive
    bool SamePath(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty())
            return false;
        return CompareStringOrdinal(ToWide(a).c_str(), -1, ToWide(b).c_str(), -1, TRUE) == CSTR_EQUAL;
    }
}

CPlaybackQueueExtension::CPlaybackQueueExtension(MyPlugin* plugin, DWORD mainThreadId)
    : _plugin(plugin), _mainThreadId(mainThreadId)
{
}

CPlaybackQueueExtension::~CPlaybackQueueExtension()
{
    ReleaseSlotLocked();
}

// =========================================================================
// IUnknown
// =========================================================================

HRESULT WINAPI CPlaybackQueueExtension::QueryInterface(REFIID riid, void** ppvObject)
{
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAIMPExtensionPlaybackQueue))
    {
        *ppvObject = static_cast<IAIMPExtensionPlaybackQueue*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG WINAPI CPlaybackQueueExtension::AddRef()
{
    return InterlockedIncrement(&_refCount);
}

ULONG WINAPI CPlaybackQueueExtension::Release()
{
    ULONG count = InterlockedDecrement(&_refCount);
    if (count == 0)
        delete this;
    return count;
}

// =========================================================================
// IAIMPExtensionPlaybackQueue
// =========================================================================

HRESULT WINAPI CPlaybackQueueExtension::GetNext(IUnknown* Current, LongWord Flags, IAIMPPlaybackQueueItem* QueueItem)
{
    long long startedAt = NowMs();
    DWORD threadId = GetCurrentThreadId();
    json entry = {
        {"call", "GetNext"},
        {"t", startedAt},
        {"threadId", threadId},
        {"mainThread", threadId == _mainThreadId},
        {"flags", Flags},
        {"current", ItemPath(Current)},
    };

    NextTrackRequest slot;
    IAIMPPlaylistItem* item = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_slotItem)
        {
            slot = _slot;
            item = _slotItem;
            item->AddRef();
        }
    }

    if (!item)
    {
        entry["result"] = "no_slot";
        Log(entry);
        return E_FAIL;
    }

    if (slot.delayMs > 0)
        Sleep(static_cast<DWORD>(slot.delayMs));

    HRESULT hr = QueueItem->SetValueAsObject(AIMP_PLAYBACKQUEUEITEM_PROPID_PLAYLISTITEM, item);
    item->Release();
    if (SUCCEEDED(hr) && slot.offsetMs > 0)
    {
        entry["offsetHr"] = HrText(QueueItem->SetValueAsFloat(AIMP_PLAYBACKQUEUEITEM_PROPID_OFFSET, slot.offsetMs / 1000.0));
    }

    entry["path"] = slot.path;
    entry["switching"] = ApplyAutoSwitching(slot);
    entry["hr"] = HrText(hr);
    entry["result"] = SUCCEEDED(hr) ? "slot" : "set_item_failed";
    entry["elapsedMs"] = NowMs() - startedAt;
    Log(entry);

    return SUCCEEDED(hr) ? S_OK : E_FAIL;
}

HRESULT WINAPI CPlaybackQueueExtension::GetPrev(IUnknown* Current, LongWord Flags, IAIMPPlaybackQueueItem* QueueItem)
{
    DWORD threadId = GetCurrentThreadId();
    Log({{"call", "GetPrev"},
         {"threadId", threadId},
         {"mainThread", threadId == _mainThreadId},
         {"flags", Flags},
         {"current", ItemPath(Current)},
         {"result", "not_handled"}});
    return E_FAIL;
}

void WINAPI CPlaybackQueueExtension::OnSelect(IAIMPPlaylistItem* Item, IAIMPPlaybackQueueItem* QueueItem)
{
    DWORD threadId = GetCurrentThreadId();
    Log({{"call", "OnSelect"},
         {"threadId", threadId},
         {"mainThread", threadId == _mainThreadId},
         {"item", ItemPath(Item)},
         {"queueItem", ItemPath(QueueItem)}});
}

// =========================================================================
// Slot management (main thread)
// =========================================================================

HRESULT CPlaybackQueueExtension::SetRegistered(bool enabled)
{
    IAIMPCore* core = _plugin->GetCore();
    if (!core)
        return E_FAIL;

    HRESULT hr = S_OK;
    if (enabled && !_registered)
    {
        hr = core->RegisterExtension(IID_IAIMPServicePlaybackQueue, static_cast<IAIMPExtensionPlaybackQueue*>(this));
        _registered = SUCCEEDED(hr);
    }
    else if (!enabled && _registered)
    {
        hr = core->UnregisterExtension(static_cast<IAIMPExtensionPlaybackQueue*>(this));
        _registered = false;
    }

    Log({{"call", "SetRegistered"}, {"enabled", enabled}, {"registered", _registered.load()}, {"hr", HrText(hr)}});
    return hr;
}

HRESULT CPlaybackQueueExtension::PrepareSlot(const NextTrackRequest& request, std::string& error)
{
    if (GetFileAttributesW(ToWide(request.path).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = "File not found";
        return E_INVALIDARG;
    }

    IAIMPServicePlaylistManager* manager = _plugin->GetPlaylistService();
    IAIMPString* name = nullptr;
    if (!manager || FAILED(_plugin->CreateAIMPString(MANAGED_PLAYLIST_NAME, &name)))
    {
        error = "Playlist service unavailable";
        return E_FAIL;
    }

    IAIMPPlaylist* playlist = nullptr;
    HRESULT hr = manager->GetLoadedPlaylistByName(name, &playlist);
    if (FAILED(hr))
        hr = manager->CreatePlaylist(name, FALSE, &playlist);
    name->Release();
    if (FAILED(hr))
    {
        error = "Failed to open the managed playlist (" + HrText(hr) + ")";
        return hr;
    }

    IAIMPString* file = nullptr;
    hr = _plugin->CreateAIMPString(request.path, &file);
    if (SUCCEEDED(hr))
    {
        // NOTHREADING: the item must exist when Add returns so it can be handed to GetNext
        hr = playlist->Add(file, AIMP_PLAYLIST_ADD_FLAGS_NOTHREADING, -1);
        file->Release();
    }

    IAIMPPlaylistItem* item = nullptr;
    int count = playlist->GetItemCount();
    if (SUCCEEDED(hr) && count > 0)
        hr = playlist->GetItem(count - 1, IID_IAIMPPlaylistItem, (void**)&item);
    playlist->Release();

    std::string itemPath = item ? ItemPath(item) : "";
    if (FAILED(hr) || !item || !SamePath(itemPath, request.path))
    {
        if (item)
            item->Release();
        error = "Added item not found at the end of the managed playlist (" + HrText(hr) + ", last item: " + itemPath + ")";
        return FAILED(hr) ? hr : E_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        ReleaseSlotLocked();
        _slot = request;
        _slotItem = item;
    }

    Log({{"call", "PrepareSlot"},
         {"threadId", GetCurrentThreadId()},
         {"mainThread", GetCurrentThreadId() == _mainThreadId},
         {"path", request.path},
         {"playlistItemCount", count}});
    return S_OK;
}

void CPlaybackQueueExtension::ClearSlot()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ReleaseSlotLocked();
    }
    Log({{"call", "ClearSlot"}});
}

HRESULT CPlaybackQueueExtension::NotifyChanged()
{
    IAIMPServicePlaybackQueue2* queueService = nullptr;
    HRESULT hr = _plugin->GetCore()->QueryInterface(IID_IAIMPServicePlaybackQueue2, (void**)&queueService);
    if (SUCCEEDED(hr))
    {
        queueService->NotifyChanged(this);
        queueService->Release();
    }
    Log({{"call", "NotifyChanged"}, {"hr", HrText(hr)}});
    return hr;
}

// =========================================================================
// Any thread
// =========================================================================

void CPlaybackQueueExtension::OnStreamStart(const std::string& playingPath)
{
    bool slotStarted = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_slotItem && SamePath(_slot.path, playingPath))
        {
            ReleaseSlotLocked();
            slotStarted = true;
        }
    }
    Log({{"event", "stream_start"}, {"path", playingPath}, {"slotStarted", slotStarted}});
}

json CPlaybackQueueExtension::GetState()
{
    std::lock_guard<std::mutex> lock(_mutex);
    json slot = nullptr;
    if (_slotItem)
    {
        slot = {
            {"path", _slot.path},
            {"offsetMs", _slot.offsetMs},
            {"crossfadeMs", _slot.crossfadeMs},
            {"fadeInMs", _slot.fadeInMs},
            {"fadeOutMs", _slot.fadeOutMs},
            {"delayMs", _slot.delayMs},
        };
    }
    return {
        {"registered", _registered.load()},
        {"mainThreadId", _mainThreadId},
        {"slot", slot},
        {"log", json(_log)},
    };
}

// =========================================================================
// Private helpers
// =========================================================================

void CPlaybackQueueExtension::Log(json entry)
{
    if (!entry.contains("t"))
        entry["t"] = NowMs();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _log.push_back(entry);
        if (_log.size() > LOG_CAPACITY)
            _log.pop_front();
    }
    _plugin->BroadcastWS({{"event", "poc_queue"}, {"data", entry}});
}

void CPlaybackQueueExtension::ReleaseSlotLocked()
{
    if (_slotItem)
    {
        _slotItem->Release();
        _slotItem = nullptr;
    }
    _slot = NextTrackRequest();
}

// File name of a playlist item or playback queue item; "" when unknown
std::string CPlaybackQueueExtension::ItemPath(IUnknown* obj)
{
    if (!obj)
        return "";

    std::string path;
    IAIMPPlaylistItem* item = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_IAIMPPlaylistItem, (void**)&item)))
    {
        path = _plugin->GetPropertyText(item, AIMP_PLAYLISTITEM_PROPID_FILENAME, "");
        item->Release();
        return path;
    }

    IAIMPPlaybackQueueItem* queueItem = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_IAIMPPlaybackQueueItem, (void**)&queueItem)))
    {
        if (SUCCEEDED(queueItem->GetValueAsObject(AIMP_PLAYBACKQUEUEITEM_PROPID_PLAYLISTITEM, IID_IAIMPPlaylistItem, (void**)&item)))
        {
            path = _plugin->GetPropertyText(item, AIMP_PLAYLISTITEM_PROPID_FILENAME, "");
            item->Release();
        }
        queueItem->Release();
        return path.empty() ? "(queue item)" : path;
    }

    return "(unknown object)";
}

// Writes the slot's fade values into the player's auto-switching settings; returns HRESULT per property
json CPlaybackQueueExtension::ApplyAutoSwitching(const NextTrackRequest& slot)
{
    json result = json::object();
    if (slot.crossfadeMs < 0 && slot.fadeInMs < 0 && slot.fadeOutMs < 0)
        return result;

    IAIMPPropertyList* props = nullptr;
    HRESULT hr = _plugin->GetPlayerService()->QueryInterface(IID_IAIMPPropertyList, (void**)&props);
    if (FAILED(hr))
    {
        result["propertyList"] = HrText(hr);
        return result;
    }

    if (slot.crossfadeMs >= 0)
        result["crossfadeMs"] = HrText(props->SetValueAsInt32(AIMP_PLAYER_PROPID_AUTOSWITCHING_CROSSFADE, slot.crossfadeMs));
    if (slot.fadeInMs >= 0)
        result["fadeInMs"] = HrText(props->SetValueAsInt32(AIMP_PLAYER_PROPID_AUTOSWITCHING_FADEIN, slot.fadeInMs));
    if (slot.fadeOutMs >= 0)
        result["fadeOutMs"] = HrText(props->SetValueAsInt32(AIMP_PLAYER_PROPID_AUTOSWITCHING_FADEOUT, slot.fadeOutMs));
    props->Release();
    return result;
}
