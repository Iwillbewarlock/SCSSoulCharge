#include "core/charge_rules.h"

#include <algorithm>
#include <cmath>

namespace scssc::core
{
	namespace
	{
		constexpr float kEpsilon = 0.001f;

		bool ValidAmount(float a_value)
		{
			return std::isfinite(a_value) && a_value >= 0.0f;
		}
	}

	bool AllowFireAndForget(float a_charge, float a_cost)
	{
		return ValidAmount(a_charge) && ValidAmount(a_cost) && a_charge + kEpsilon >= a_cost;
	}

	float FireAndForgetDebit(float a_cost, bool a_targetLocationFailed)
	{
		if (a_targetLocationFailed || !ValidAmount(a_cost)) {
			return 0.0f;
		}
		return a_cost;
	}

	bool AllowConcentrationStart(float a_charge, float a_costPerSecond)
	{
		if (!ValidAmount(a_charge) || !ValidAmount(a_costPerSecond)) {
			return false;
		}
		return a_costPerSecond == 0.0f || a_charge > kEpsilon;
	}

	ChannelTick ConcentrationTick(float a_charge, float a_costPerSecond, float a_deltaSeconds)
	{
		if (!ValidAmount(a_charge) || !ValidAmount(a_costPerSecond) || !std::isfinite(a_deltaSeconds) ||
			a_deltaSeconds < 0.0f || a_deltaSeconds > kMaxBilledFrameSeconds) {
			return { 0.0f, true };
		}
		if (a_costPerSecond == 0.0f) {
			return {};
		}
		const float due = a_costPerSecond * a_deltaSeconds;
		const float debit = std::min(due, a_charge);
		const bool  empty = a_charge - debit <= kEpsilon;
		return { debit, empty };
	}
}
