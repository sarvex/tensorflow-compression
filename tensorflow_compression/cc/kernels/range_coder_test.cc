/* Copyright 2018 Google LLC. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cmath>
#include <random>

#include "tensorflow/core/lib/random/distribution_sampler.h"
#include "tensorflow/core/lib/random/philox_random.h"
#include "tensorflow/core/lib/random/simple_philox.h"
#include "tensorflow/core/platform/stacktrace_handler.h"
#include "tensorflow/core/platform/test.h"

#include "tensorflow_compression/cc/kernels/range_coder.h"

namespace tensorflow_compression {
namespace {
namespace random = tensorflow::random;

void RangeEncodeDecodeTest(int precision, random::SimplePhilox* gen) {
  constexpr int kAlphabetSize = 256;

  std::vector<float> distribution_weight;
  distribution_weight.reserve(kAlphabetSize);
  for (int i = 1; i <= kAlphabetSize; ++i) {
    distribution_weight.push_back(std::pow(static_cast<float>(i), -2.0f));
  }

  random::DistributionSampler sampler(distribution_weight);

  const int multiplier = (precision > 7) ? 32 : 1;
  std::vector<int32> histogram(kAlphabetSize, multiplier - 1);

  const int data_size =
      (multiplier << precision) - histogram.size() * (multiplier - 1);
  CHECK_GE(data_size, 0);
  std::vector<uint8> data(data_size);
  for (uint8& x : data) {
    x = sampler.Sample(gen);
    ++histogram[x];
  }

  std::vector<int32> cdf(histogram.size() + 1, 0);
  int partial_sum = 0;
  for (int i = 0; i < histogram.size(); ++i) {
    partial_sum += histogram[i];
    cdf[i + 1] = partial_sum / multiplier;
  }

  ASSERT_EQ(cdf.front(), 0);
  ASSERT_EQ(cdf.back(), 1 << precision);

  std::vector<double> ideal_code_length(histogram.size());
  const double normalizer = static_cast<double>(1 << precision);
  for (int i = 0; i < ideal_code_length.size(); ++i) {
    ideal_code_length[i] = -std::log2((cdf[i + 1] - cdf[i]) / normalizer);
  }

  RangeEncoder encoder;
  tensorflow::tstring encoded;
  double ideal_length = 0.0;
  for (uint8 x : data) {
    encoder.Encode(cdf[x], cdf[x + 1], precision, &encoded);
    ideal_length += ideal_code_length[x];
  }
  encoder.Finalize(&encoded);

  LOG(INFO) << "Encoded string length (bits): " << 8 * encoded.size()
            << ", whereas ideal " << ideal_length << " ("
            << (8 * encoded.size()) / ideal_length << " of ideal) "
            << " (ideal compression rate " << ideal_length / (8 * data.size())
            << ")";

  RangeDecoder decoder(encoded);
  for (int i = 0; i < data.size(); ++i) {
    const int32 decoded = decoder.Decode(cdf, precision);
    ASSERT_EQ(decoded, static_cast<int32>(data[i])) << i;
  }
}

TEST(RangeCoderTest, Precision1To11) {
  std::random_device rd;
  random::PhiloxRandom gen(rd(), rd());
  random::SimplePhilox rand(&gen);
  const int precision = 1 + rand.Uniform(11);
  RangeEncodeDecodeTest(precision, &rand);
}

TEST(RangeCoderTest, Precision12To16) {
  std::random_device rd;
  random::PhiloxRandom gen(rd(), rd());
  random::SimplePhilox rand(&gen);
  for (int precision = 12; precision < 17; ++precision) {
    RangeEncodeDecodeTest(precision, &rand);
  }
}

TEST(RangeCoderTest, FinalizeState0) {
  constexpr int kPrecision = 2;

  tensorflow::tstring output;
  RangeEncoder encoder;
  encoder.Encode(0, 2, kPrecision, &output);
  encoder.Finalize(&output);

  RangeDecoder decoder(output);
  EXPECT_EQ(decoder.Decode({0, 2, 4}, kPrecision), 0);
}

}  // namespace
}  // namespace tensorflow_compression

GTEST_API_ int main(int argc, char** argv) {
  tensorflow::testing::InstallStacktraceHandler();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
