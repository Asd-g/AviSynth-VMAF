#include "VMAF.h"

const char *AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment *env)
{
    avs_add_function(env, "VMAF", "ccs[log_format]i[model]i*[feature]i*[cambi_opt]s", Create_VMAF, 0);
    avs_add_function(env, "VMAF2", "c[distorted]c[feature]i*[cambi_opt]s", Create_VMAF2, 0);
    return "VMAF";
}
