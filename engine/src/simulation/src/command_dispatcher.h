#ifndef SIMULATION_SRC_COMMAND_DISPATCHER_H_
#define SIMULATION_SRC_COMMAND_DISPATCHER_H_

#include "simulation/public/command.h"

namespace network_example {

class KernelEngine;

namespace simulation {

class Dispatcher {
public:
    CommandResult dispatch(KernelEngine& engine, const Command& command) const;
};

}  // namespace simulation
}  // namespace network_example

#endif  // SIMULATION_SRC_COMMAND_DISPATCHER_H_
