// SPDX-License-Identifier: Apache-2.0
//
// Fan-out for the per-group tool registration functions.

#include "agent_internal.h"

void agent_register_builtin_tools(void)
{
    agent_tools_register_wifi();
    agent_tools_register_gps();
    agent_tools_register_lora();
    agent_tools_register_nfc();
    agent_tools_register_fs();
    agent_tools_register_net();
    agent_tools_register_sys();
}
