#include <obs-module.h>
#include <plugin-support.h>
#include "audio-sender.h"
#include "audio-receiver.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-audio-router", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS Audio Router - Send audio tracks between two PCs over LAN";
}

bool obs_module_load(void)
{
    obs_log(LOG_INFO, "obs-audio-router loaded (version %s)", PLUGIN_VERSION);
    register_audio_sender_filter();
    register_audio_receiver_source();
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "obs-audio-router unloaded");
}
