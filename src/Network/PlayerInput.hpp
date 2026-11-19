#pragma once

#include "WireProtocol.hpp"

#include <Physics/PlayerController.hpp>

namespace net
{
inline physics::PlayerInput playerInput(const wire::Input &input)
{
	return {{input.moveX, input.moveZ},
		(input.flags & wire::Input::sprint) != 0, (input.flags & wire::Input::jump) != 0,
		(input.flags & wire::Input::swimUp) != 0, (input.flags & wire::Input::swimDown) != 0};
}
} // namespace net
