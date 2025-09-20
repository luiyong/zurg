#pragma once

#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <memory>
#include <string>

namespace ops { namespace v1 { class Control; } }

namespace zurg { namespace agent {

void StartAgent(ops::v1::Control::Stub* stub, const std::string& agent_id);

} }
