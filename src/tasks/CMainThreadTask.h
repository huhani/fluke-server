#pragma once
#include <functional>
#include "../core/Plugin.h"

/**
 * @class CMainThreadTask
 * @brief IAIMPTask that runs an arbitrary callable on AIMP's main thread.
 *
 * Lets a route handler run several AIMP calls in one main-thread hop instead of
 * writing a dedicated task class per request.
 */
class CMainThreadTask : public IAIMPTask
{
private:
    volatile ULONG _refCount = 1;
    std::function<void()> _fn;

public:
    explicit CMainThreadTask(std::function<void()> fn) : _fn(std::move(fn)) {}

    HRESULT WINAPI QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAIMPTask))
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG WINAPI AddRef() override
    {
        return InterlockedIncrement(&_refCount);
    }

    ULONG WINAPI Release() override
    {
        ULONG count = InterlockedDecrement(&_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    void WINAPI Execute(IAIMPTaskOwner* Owner) override
    {
        _fn();
    }
};

/**
 * @brief Runs `fn` on AIMP's main thread and waits until it has finished.
 * @return HRESULT of ExecuteInMainThread (E_FAIL when the thread service is unavailable)
 */
inline HRESULT RunInMainThread(MyPlugin* plugin, std::function<void()> fn)
{
    IAIMPServiceThreads* threads = plugin->GetThreadService();
    if (!threads)
        return E_FAIL;

    CMainThreadTask* task = new CMainThreadTask(std::move(fn));
    HRESULT hr = threads->ExecuteInMainThread(task, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);
    task->Release();
    return hr;
}
