#include "buffered_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "resampler.h"

namespace chromemedia {
namespace codec {

std::unique_ptr<BufferedResampler> BufferedResampler::Create(
    int internal_sample_rate, int external_sample_rate) {
  auto resampler =
      Resampler::Create(internal_sample_rate, external_sample_rate);
  if (resampler == nullptr) {
    std::cerr << "Could not create Resampler." << std::endl;
    return nullptr;
  }

  return absl::WrapUnique(new BufferedResampler(std::move(resampler)));
}

BufferedResampler::BufferedResampler(
    std::unique_ptr<ResamplerInterface> resampler)
    : leftover_samples_(0), resampler_(std::move(resampler)) {
  if (resampler_->target_sample_rate_hz() >
      resampler_->input_sample_rate_hz()) {
    if (resampler_->target_sample_rate_hz() %
                 resampler_->input_sample_rate_hz() !=
             0) {
      std::cerr << "Resampler target sample rate must be a multiple of the "
                   "input sample rate.";
      exit(1);
    }

    leftover_samples_.reserve(resampler_->target_sample_rate_hz() /
                                  resampler_->input_sample_rate_hz() -
                              1);
  } else {
    if (resampler_->input_sample_rate_hz() %
                 resampler_->target_sample_rate_hz() !=
             0) {
        std::cerr << "Resampler input sample rate must be a multiple of the "
                         "target sample rate.";
        exit(1);
    }
  }
}

std::optional<std::vector<int16_t>> BufferedResampler::FilterAndBuffer(
    const std::function<std::optional<std::vector<int16_t>>(int)>&
        sample_generator,
    int num_external_samples_requested) {
  const int num_internal_samples_to_generate =
      GetInternalNumSamplesToGenerate(num_external_samples_requested);

  // 1. If we have any leftover samples from last time we must use them.
  std::vector<int16_t> samples(num_external_samples_requested);
  const int num_leftover_used =
      UseLeftoverSamples(num_external_samples_requested, &samples);

  // 2. Generate samples using |sample_generator|.
  auto internal_samples = sample_generator(num_internal_samples_to_generate);
  if (!internal_samples.has_value()) {
    return std::nullopt;
  }
  if (internal_samples->size() != num_internal_samples_to_generate) {
    std::cerr << "Resampler did not generate the expected number of samples.";
    exit(1);
  }

  // 3. Resample the internal samples to produce new samples.
  const std::vector<int16_t> external_samples =
      Resample(internal_samples.value());

  // 4. Copy the new samples to output and the leftover buffers.
  CopyNewSamples(external_samples, num_external_samples_requested,
                 num_leftover_used, &samples);
  return samples;
}

int BufferedResampler::GetInternalNumSamplesToGenerate(
    int num_external_samples_requested) const {
  if (num_external_samples_requested <= leftover_samples_.size()) {
    return 0;
  }
  const int new_external_samples_needed =
      num_external_samples_requested - leftover_samples_.size();
  const float resample_ratio =
      static_cast<float>(resampler_->target_sample_rate_hz()) /
      static_cast<float>(resampler_->input_sample_rate_hz());

  return static_cast<int>(std::ceil(
      static_cast<float>(new_external_samples_needed) / resample_ratio));
}

int BufferedResampler::UseLeftoverSamples(int num_external_samples_requested,
                                          std::vector<int16_t>* samples) {
  const int num_leftover_used =
      std::min(static_cast<int>(leftover_samples_.size()),
               num_external_samples_requested);
  std::move(leftover_samples_.begin(),
            leftover_samples_.begin() + num_leftover_used, samples->begin());
  std::move(leftover_samples_.begin() + num_leftover_used,
            leftover_samples_.end(), leftover_samples_.begin());
  leftover_samples_.resize(leftover_samples_.size() - num_leftover_used);
  return num_leftover_used;
}

std::vector<int16_t> BufferedResampler::Resample(
    const std::vector<int16_t>& internal_samples) {
  // If the internal and external sample rates match, no need to do anything.
  if (resampler_->target_sample_rate_hz() ==
      resampler_->input_sample_rate_hz()) {
    return internal_samples;
  }
  return resampler_->Resample(internal_samples);
}

void BufferedResampler::CopyNewSamples(
    const std::vector<int16_t>& external_samples,
    int num_external_samples_requested, int num_leftover_used,
    std::vector<int16_t>* samples) {
  // Copy the needed samples to the destination, which already has some
  // leftover samples from the last run.
  const int num_samples_to_copy =
      num_external_samples_requested - num_leftover_used;
  if (external_samples.size() < num_samples_to_copy) {
        std::cerr << "Number of external samples is less than num samples to copy.";
        exit(1);
  }

  std::copy(external_samples.begin(),
            external_samples.begin() + num_samples_to_copy,
            samples->begin() + num_leftover_used);

  // Store the rest in the |leftover_samples_|.
  leftover_samples_.insert(leftover_samples_.end(),
                           external_samples.begin() + num_samples_to_copy,
                           external_samples.end());
}

}  // namespace codec
}  // namespace chromemedia