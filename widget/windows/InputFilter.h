/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_windows_InputFilter_h
#define widget_windows_InputFilter_h

namespace mozilla {
namespace widget {

// Simple flag to block native mouse input in Firefox
// When enabled, nsWindow skips processing native mouse messages
class InputFilter {
 public:
  static void Enable();
  static void Disable();
  static bool IsEnabled();

 private:
  static bool sEnabled;
};

}  // namespace widget
}  // namespace mozilla

#endif  // widget_windows_InputFilter_h
