#pragma once

#include <cstddef>

constexpr size_t COMMAND_MESSAGE_MAX_LENGTH = 512;

struct CommandMessage
{
  size_t length = 0;
  char payload[COMMAND_MESSAGE_MAX_LENGTH + 1] = {0};
};
