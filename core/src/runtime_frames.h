#pragma once

#include "exec/executor.h"
#include "lyflow/c_api.h"

namespace lyflow::runtime {
std::vector<exec::InjectedInput> frameInputs(const lyflow_frame* frame,
                                             const std::string& bindings);
}
