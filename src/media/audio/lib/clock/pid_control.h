// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_CLOCK_PID_CONTROL_H_
#define SRC_MEDIA_AUDIO_LIB_CLOCK_PID_CONTROL_H_

#include <zircon/time.h>

namespace media::audio {

class PidControl {
  friend class PidControlTest;

 public:
  PidControl(double p_val, double i_val, double d_val)
      : proportional_factor_(p_val), integral_factor_(i_val), derivative_factor_(d_val) {
    Start(0);
  }
  PidControl() : PidControl(0.0, 0.0, 0.0) {}

  void Start(zx_time_t start_time);

  double Read();
  void TuneForError(zx_time_t time, double error);

  void DisplayCoefficients();

 private:
  double proportional_factor_;
  double integral_factor_;
  double derivative_factor_;

  double prop_contrib_;
  double integ_contrib_;
  double deriv_contrib_;
  double total_pid_contribution_;

  zx_time_t tune_time_;
  double current_error_;
  double accum_error_;
  double delta_error_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_LIB_CLOCK_PID_CONTROL_H_
