#pragma once

namespace maya {

/// Dock and minimized-window icon. `cocoa_window` is an `NSWindow*`, or null.
/// Main thread only. A missing or unreadable logo leaves the system icon in place.
void set_application_icon(void* cocoa_window);

} // namespace maya
