#pragma once

#include "command_message.h"

class BleCommandProcessor
{
public:
  void begin();
  void handleCommand(const CommandMessage &message) const;
  void pollConnection();
  void sendReadyEvent() const;
  void applyIdentityFromConfig();
};
