// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/model/measurement.hpp"

namespace latobs::model {

std::string measurement_identity_text(SourceId source, EpochId epoch, IncarnationId incarnation,
                                      Sequence sequence) {
  std::string text = "measurement|";
  text.append(source.to_hex());
  text.push_back('|');
  text.append(epoch.to_hex());
  text.push_back('|');
  text.append(incarnation.to_hex());
  text.push_back('|');
  text.append(std::to_string(sequence.value()));
  return text;
}

MeasurementId derive_measurement_id(SourceId source, EpochId epoch, IncarnationId incarnation,
                                    Sequence sequence) noexcept {
  const std::string text = measurement_identity_text(source, epoch, incarnation, sequence);
  return MeasurementId::derive_from(text);
}

}  // namespace latobs::model
