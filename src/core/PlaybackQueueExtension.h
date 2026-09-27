#pragma once

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "Plugin.h"
#include "../../sdk/aimp/5.40/apiPlayer.h"
#include "../../sdk/aimp/5.40/apiPlaylists.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @brief Formats an HRESULT as "0x80004005" for logs and error responses
 */
inline std::string HrText(HRESULT hr)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

/**
 * @brief Next track handed to AIMP by the playback client (PoC).
 *
 * Time values are milliseconds. A negative fade value leaves that player setting untouched.
 */
struct NextTrackRequest
{
    std::string path;
    int offsetMs = 0;
    int crossfadeMs = -1;
    int fadeInMs = -1;
    int fadeOutMs = -1;
    /// Artificial delay inside GetNext, to measure how long AIMP tolerates a slow answer.
    int delayMs = 0;
};

/**
 * @class CPlaybackQueueExtension
 * @brief PoC IAIMPExtensionPlaybackQueue: AIMP asks this extension which track plays next.
 *
 * GetNext keeps returning the prepared slot (an item of the "musicbank" playlist) until that
 * file starts playing, so repeated calls get the same answer. With an empty slot it returns
 * E_FAIL and AIMP falls back to its own logic. Every call is logged (ring buffer + WebSocket
 * `poc_queue` event) to measure call timing, calling thread and repeat count.
 *
 * The extension is not registered by default; SetRegistered(true) enables it at runtime.
 */
class CPlaybackQueueExtension : public IAIMPExtensionPlaybackQueue
{
private:
    volatile ULONG _refCount = 1;
    MyPlugin* _plugin;
    DWORD _mainThreadId;
    std::atomic<bool> _registered{false};

    std::mutex _mutex;
    NextTrackRequest _slot;
    IAIMPPlaylistItem* _slotItem = nullptr; ///< owned reference, null when the slot is empty
    std::deque<json> _log;

    void Log(json entry);
    void ReleaseSlotLocked();
    std::string ItemPath(IUnknown* obj);
    json ApplyAutoSwitching(const NextTrackRequest& slot);

public:
    CPlaybackQueueExtension(MyPlugin* plugin, DWORD mainThreadId);
    ~CPlaybackQueueExtension();

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;

    // IAIMPExtensionPlaybackQueue
    HRESULT WINAPI GetNext(IUnknown* Current, LongWord Flags, IAIMPPlaybackQueueItem* QueueItem) override;
    HRESULT WINAPI GetPrev(IUnknown* Current, LongWord Flags, IAIMPPlaybackQueueItem* QueueItem) override;
    void WINAPI OnSelect(IAIMPPlaylistItem* Item, IAIMPPlaybackQueueItem* QueueItem) override;

    // Main thread only
    HRESULT SetRegistered(bool enabled);
    HRESULT PrepareSlot(const NextTrackRequest& request, std::string& error);
    void ClearSlot();
    HRESULT NotifyChanged();

    // Any thread
    void OnStreamStart(const std::string& playingPath);
    json GetState();
};
