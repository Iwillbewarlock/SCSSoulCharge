#pragma once

namespace scssc
{
	// Every hook is in place and bindings are applied (false on an unsupported runtime, when a hook failed, when the
	// menu hook failed, or while the uninstall option runs). Any thread.
	bool IsActive();
}
