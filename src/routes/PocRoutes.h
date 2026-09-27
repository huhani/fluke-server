#pragma once

class MyPlugin;
/**
 * @brief Registers the PoC endpoints for the playback queue extension (GetNext hook)
 * @param plugin Pointer to MyPlugin instance
 *
 * Endpoints registered (all times in ms):
 * - GET    /poc/queue            - Extension state, prepared slot and the last GetNext/OnSelect/stream start log
 * - POST   /poc/queue/extension  - Register or unregister the extension (body: {"enabled": bool})
 * - POST   /poc/queue/next       - Prepare the next track (body: {"path", "offsetMs", "crossfadeMs", "fadeInMs", "fadeOutMs", "delayMs", "notify"})
 * - DELETE /poc/queue/next       - Clear the prepared track (body: {"notify": bool}, optional)
 * - GET    /poc/switching        - Read the player's track switching settings
 * - POST   /poc/switching        - Write track switching settings (body: any subset of the GET keys)
 */
void RegisterPocRoutes(MyPlugin* plugin, const std::string& prefix);
