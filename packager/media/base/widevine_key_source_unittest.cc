// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "packager/base/base64.h"
#include "packager/base/strings/string_number_conversions.h"
#include "packager/base/strings/stringprintf.h"
#include "packager/media/base/fixed_key_source.h"
#include "packager/media/base/key_fetcher.h"
#include "packager/media/base/request_signer.h"
#include "packager/media/base/test/status_test_util.h"
#include "packager/media/base/widevine_key_source.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrEq;

namespace shaka {
namespace {
const char kServerUrl[] = "http://www.foo.com/getcontentkey";
const char kContentId[] = "ContentFoo";
const char kPolicy[] = "PolicyFoo";
const char kSignerName[] = "SignerFoo";

const char kMockSignature[] = "MockSignature";

// The license service may return an error indicating a transient error has
// just happened in the server, or other types of errors.
// WidevineKeySource will perform a number of retries on transient
// errors;
// WidevineKeySource does not know about other errors and retries are
// not performed.
const char kLicenseStatusTransientError[] = "INTERNAL_ERROR";
const char kLicenseStatusUnknownError[] = "UNKNOWN_ERROR";

const char kExpectedRequestMessageFormat[] =
    "{\"content_id\":\"%s\",\"drm_types\":[\"WIDEVINE\"],\"policy\":\"%s\","
    "\"tracks\":[{\"type\":\"SD\"},{\"type\":\"HD\"},{\"type\":\"AUDIO\"}]}";
const char kExpectedRequestMessageWithAssetIdFormat[] =
    "{\"asset_id\":%u,\"drm_types\":[\"WIDEVINE\"],"
    "\"tracks\":[{\"type\":\"SD\"},{\"type\":\"HD\"},{\"type\":\"AUDIO\"}]}";
const char kExpectedRequestMessageWithPsshFormat[] =
    "{\"drm_types\":[\"WIDEVINE\"],\"pssh_data\":\"%s\","
    "\"tracks\":[{\"type\":\"SD\"},{\"type\":\"HD\"},{\"type\":\"AUDIO\"}]}";
const char kExpectedSignedMessageFormat[] =
    "{\"request\":\"%s\",\"signature\":\"%s\",\"signer\":\"%s\"}";
const char kTrackFormat[] =
    "{\"type\":\"%s\",\"key_id\":\"%s\",\"key\":"
    "\"%s\",\"pssh\":[{\"drm_type\":\"WIDEVINE\",\"data\":\"%s\"}]}";
const char kClassicTrackFormat[] = "{\"type\":\"%s\",\"key\":\"%s\"}";
const char kLicenseResponseFormat[] = "{\"status\":\"%s\",\"tracks\":[%s]}";
const char kHttpResponseFormat[] = "{\"response\":\"%s\"}";
const uint8_t kRequestPsshBox[] = {
    0,    0,    0,    41,   'p',  's',  's',  'h',  0,    0,    0,
    0,    0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce, 0xa3, 0xc8,
    0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed, 0,    0,    0,    0x09, 'P',
    'S',  'S',  'H',  ' ',  'd',  'a',  't',  'a'};
const char kRequestPsshData[] = "PSSH data";
const uint8_t kRequestPsshDataFromKeyIds[] = {0x12, 0x06, 0x00, 0x01,
                                              0x02, 0x03, 0x04, 0x05};
const uint8_t kRequestKeyId[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
// 32-bit with leading bit set, to verify that big uint32_t can be handled
// correctly.
const uint32_t kClassicAssetId = 0x80038cd9;

std::string Base64Encode(const std::string& input) {
  std::string output;
  base::Base64Encode(input, &output);
  return output;
}

std::string ToString(const std::vector<uint8_t> v) {
  return std::string(v.begin(), v.end());
}

std::string GetMockKeyId(const std::string& track_type) {
  // Key ID must be 16 characters.
  std::string key_id = "MockKeyId" + track_type;
  key_id.resize(16, '~');
  return key_id;
}

std::string GetMockKey(const std::string& track_type) {
  return "MockKey" + track_type;
}

std::string GetMockPsshData(const std::string& track_type) {
  return "MockPsshData" + track_type;
}

std::string GenerateMockLicenseResponse() {
  const std::string kTrackTypes[] = {"SD", "HD", "AUDIO"};
  std::string tracks;
  for (size_t i = 0; i < 3; ++i) {
    if (!tracks.empty())
      tracks += ",";
    tracks += base::StringPrintf(
        kTrackFormat,
        kTrackTypes[i].c_str(),
        Base64Encode(GetMockKeyId(kTrackTypes[i])).c_str(),
        Base64Encode(GetMockKey(kTrackTypes[i])).c_str(),
        Base64Encode(GetMockPsshData(kTrackTypes[i])).c_str());
  }
  return base::StringPrintf(kLicenseResponseFormat, "OK", tracks.c_str());
}

std::string GenerateMockClassicLicenseResponse() {
  const std::string kTrackTypes[] = {"SD", "HD", "AUDIO"};
  std::string tracks;
  for (size_t i = 0; i < 3; ++i) {
    if (!tracks.empty())
      tracks += ",";
    tracks += base::StringPrintf(
        kClassicTrackFormat,
        kTrackTypes[i].c_str(),
        Base64Encode(GetMockKey(kTrackTypes[i])).c_str());
  }
  return base::StringPrintf(kLicenseResponseFormat, "OK", tracks.c_str());
}

}  // namespace

namespace media {

class MockRequestSigner : public RequestSigner {
 public:
  explicit MockRequestSigner(const std::string& signer_name)
      : RequestSigner(signer_name) {}
  ~MockRequestSigner() override {}

  MOCK_METHOD2(GenerateSignature,
               bool(const std::string& message, std::string* signature));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockRequestSigner);
};

class MockKeyFetcher : public KeyFetcher {
 public:
  MockKeyFetcher() : KeyFetcher() {}
  ~MockKeyFetcher() override {}

  MOCK_METHOD3(FetchKeys,
               Status(const std::string& service_address,
                      const std::string& data,
                      std::string* response));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockKeyFetcher);
};

class WidevineKeySourceTest : public ::testing::Test,
                              public ::testing::WithParamInterface<bool> {
 public:
  WidevineKeySourceTest()
      : mock_request_signer_(new MockRequestSigner(kSignerName)),
        mock_key_fetcher_(new MockKeyFetcher()) {}

  void SetUp() override {
    content_id_.assign(
        reinterpret_cast<const uint8_t*>(kContentId),
        reinterpret_cast<const uint8_t*>(kContentId) + strlen(kContentId));
  }

 protected:
  void CreateWidevineKeySource() {
    widevine_key_source_.reset(new WidevineKeySource(kServerUrl, GetParam()));
    widevine_key_source_->set_key_fetcher(mock_key_fetcher_.Pass());
  }

  void VerifyKeys(bool classic) {
    EncryptionKey encryption_key;
    const std::string kTrackTypes[] = {"SD", "HD", "AUDIO"};
    for (size_t i = 0; i < arraysize(kTrackTypes); ++i) {
      ASSERT_OK(widevine_key_source_->GetKey(
          KeySource::GetTrackTypeFromString(kTrackTypes[i]),
          &encryption_key));
      EXPECT_EQ(GetMockKey(kTrackTypes[i]), ToString(encryption_key.key));
      if (!classic) {
        ASSERT_EQ(GetParam() ? 2u : 1u, encryption_key.key_system_info.size());
        EXPECT_EQ(GetMockKeyId(kTrackTypes[i]),
                  ToString(encryption_key.key_id));
        EXPECT_EQ(GetMockPsshData(kTrackTypes[i]),
                  ToString(encryption_key.key_system_info[0].pssh_data()));

        if (GetParam()) {
          // Each of the keys contains all the key IDs.
          const std::vector<uint8_t> common_system_id(
              kCommonSystemId, kCommonSystemId + arraysize(kCommonSystemId));
          ASSERT_EQ(common_system_id,
                    encryption_key.key_system_info[1].system_id());

          const std::vector<std::vector<uint8_t>>& key_ids =
              encryption_key.key_system_info[1].key_ids();
          ASSERT_EQ(arraysize(kTrackTypes), key_ids.size());
          for (size_t j = 0; j < arraysize(kTrackTypes); ++j) {
            // Because they are stored in a std::set, the order may change.
            const std::string key_id_str = GetMockKeyId(kTrackTypes[j]);
            const std::vector<uint8_t> key_id(key_id_str.begin(),
                                              key_id_str.end());
            EXPECT_THAT(key_ids, testing::Contains(key_id));
          }
        }
      }
    }
  }
  scoped_ptr<MockRequestSigner> mock_request_signer_;
  scoped_ptr<MockKeyFetcher> mock_key_fetcher_;
  scoped_ptr<WidevineKeySource> widevine_key_source_;
  std::vector<uint8_t> content_id_;

 private:
  DISALLOW_COPY_AND_ASSIGN(WidevineKeySourceTest);
};

TEST_P(WidevineKeySourceTest, GetTrackTypeFromString) {
  EXPECT_EQ(KeySource::TRACK_TYPE_SD,
            KeySource::GetTrackTypeFromString("SD"));
  EXPECT_EQ(KeySource::TRACK_TYPE_HD,
            KeySource::GetTrackTypeFromString("HD"));
  EXPECT_EQ(KeySource::TRACK_TYPE_AUDIO,
            KeySource::GetTrackTypeFromString("AUDIO"));
  EXPECT_EQ(KeySource::TRACK_TYPE_UNKNOWN,
            KeySource::GetTrackTypeFromString("FOO"));
}

TEST_P(WidevineKeySourceTest, GenerateSignatureFailure) {
  EXPECT_CALL(*mock_request_signer_, GenerateSignature(_, _))
      .WillOnce(Return(false));

  CreateWidevineKeySource();
  widevine_key_source_->set_signer(mock_request_signer_.Pass());
  ASSERT_EQ(Status(error::INTERNAL_ERROR, "Signature generation failed."),
            widevine_key_source_->FetchKeys(content_id_, kPolicy));
}

// Check whether expected request message and post data was generated and
// verify the correct behavior on http failure.
TEST_P(WidevineKeySourceTest, HttpFetchFailure) {
  std::string expected_message = base::StringPrintf(
      kExpectedRequestMessageFormat, Base64Encode(kContentId).c_str(), kPolicy);
  EXPECT_CALL(*mock_request_signer_,
              GenerateSignature(StrEq(expected_message), _))
      .WillOnce(DoAll(SetArgPointee<1>(kMockSignature), Return(true)));

  std::string expected_post_data =
      base::StringPrintf(kExpectedSignedMessageFormat,
                         Base64Encode(expected_message).c_str(),
                         Base64Encode(kMockSignature).c_str(),
                         kSignerName);
  const Status kMockStatus = Status::UNKNOWN;
  EXPECT_CALL(*mock_key_fetcher_,
              FetchKeys(StrEq(kServerUrl), expected_post_data, _))
      .WillOnce(Return(kMockStatus));

  CreateWidevineKeySource();
  widevine_key_source_->set_signer(mock_request_signer_.Pass());
  ASSERT_EQ(kMockStatus,
            widevine_key_source_->FetchKeys(content_id_, kPolicy));
}

TEST_P(WidevineKeySourceTest, LicenseStatusCencOK) {
  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(GenerateMockLicenseResponse()).c_str());

  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  ASSERT_OK(widevine_key_source_->FetchKeys(content_id_, kPolicy));
  VerifyKeys(false);
}

TEST_P(WidevineKeySourceTest, LicenseStatusCencNotOK) {
  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(
          GenerateMockClassicLicenseResponse()).c_str());

  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  ASSERT_EQ(error::SERVER_ERROR,
            widevine_key_source_->FetchKeys(content_id_, kPolicy)
            .error_code());
}

TEST_P(WidevineKeySourceTest, LicenseStatusCencWithPsshBoxOK) {
  std::string expected_message =
      base::StringPrintf(kExpectedRequestMessageWithPsshFormat,
                         Base64Encode(kRequestPsshData).c_str());
  EXPECT_CALL(*mock_request_signer_,
              GenerateSignature(StrEq(expected_message), _))
      .WillOnce(DoAll(SetArgPointee<1>(kMockSignature), Return(true)));

  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(GenerateMockLicenseResponse()).c_str());
  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  widevine_key_source_->set_signer(mock_request_signer_.Pass());
  std::vector<uint8_t> pssh_box(kRequestPsshBox,
                                kRequestPsshBox + arraysize(kRequestPsshBox));
  ASSERT_OK(widevine_key_source_->FetchKeys(pssh_box));
  VerifyKeys(false);
}

TEST_P(WidevineKeySourceTest, LicenseStatusCencWithKeyIdsOK) {
  std::string expected_pssh_data(
      kRequestPsshDataFromKeyIds,
      kRequestPsshDataFromKeyIds + arraysize(kRequestPsshDataFromKeyIds));
  std::string expected_message =
      base::StringPrintf(kExpectedRequestMessageWithPsshFormat,
                         Base64Encode(expected_pssh_data).c_str());
  EXPECT_CALL(*mock_request_signer_,
              GenerateSignature(StrEq(expected_message), _))
      .WillOnce(DoAll(SetArgPointee<1>(kMockSignature), Return(true)));

  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(GenerateMockLicenseResponse()).c_str());
  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  widevine_key_source_->set_signer(mock_request_signer_.Pass());
  std::vector<std::vector<uint8_t>> key_ids;
  key_ids.push_back(std::vector<uint8_t>(
      kRequestKeyId, kRequestKeyId + arraysize(kRequestKeyId)));
  ASSERT_OK(widevine_key_source_->FetchKeys(key_ids));
  VerifyKeys(false);
}

TEST_P(WidevineKeySourceTest, LicenseStatusClassicOK) {
  std::string expected_message = base::StringPrintf(
      kExpectedRequestMessageWithAssetIdFormat, kClassicAssetId);
  EXPECT_CALL(*mock_request_signer_,
              GenerateSignature(StrEq(expected_message), _))
      .WillOnce(DoAll(SetArgPointee<1>(kMockSignature), Return(true)));

  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(
          GenerateMockClassicLicenseResponse()).c_str());
  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  widevine_key_source_->set_signer(mock_request_signer_.Pass());
  ASSERT_OK(widevine_key_source_->FetchKeys(kClassicAssetId));
  VerifyKeys(true);
}

TEST_P(WidevineKeySourceTest, RetryOnHttpTimeout) {
  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(GenerateMockLicenseResponse()).c_str());

  // Retry is expected on HTTP timeout.
  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(Return(Status(error::TIME_OUT, "")))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  ASSERT_OK(widevine_key_source_->FetchKeys(content_id_, kPolicy));
  VerifyKeys(false);
}

TEST_P(WidevineKeySourceTest, RetryOnTransientError) {
  std::string mock_license_status = base::StringPrintf(
      kLicenseResponseFormat, kLicenseStatusTransientError, "");
  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(mock_license_status).c_str());

  std::string expected_retried_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(GenerateMockLicenseResponse()).c_str());

  // Retry is expected on transient error.
  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)))
      .WillOnce(DoAll(SetArgPointee<2>(expected_retried_response),
                      Return(Status::OK)));

  CreateWidevineKeySource();
  ASSERT_OK(widevine_key_source_->FetchKeys(content_id_, kPolicy));
  VerifyKeys(false);
}

TEST_P(WidevineKeySourceTest, NoRetryOnUnknownError) {
  std::string mock_license_status = base::StringPrintf(
      kLicenseResponseFormat, kLicenseStatusUnknownError, "");
  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(mock_license_status).c_str());

  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  CreateWidevineKeySource();
  ASSERT_EQ(error::SERVER_ERROR,
            widevine_key_source_->FetchKeys(content_id_, kPolicy).error_code());
}

namespace {

const char kCryptoPeriodRequestMessageFormat[] =
    "{\"content_id\":\"%s\",\"crypto_period_count\":%u,\"drm_types\":["
    "\"WIDEVINE\"],\"first_crypto_period_index\":%u,\"policy\":\"%s\","
    "\"tracks\":[{\"type\":\"SD\"},{\"type\":\"HD\"},{\"type\":\"AUDIO\"}]}";

const char kCryptoPeriodTrackFormat[] =
    "{\"type\":\"%s\",\"key_id\":\"%s\",\"key\":"
    "\"%s\",\"pssh\":[{\"drm_type\":\"WIDEVINE\",\"data\":\"\"}], "
    "\"crypto_period_index\":%u}";

std::string GetMockKey(const std::string& track_type, uint32_t index) {
  return "MockKey" + track_type + "@" + base::UintToString(index);
}

std::string GenerateMockKeyRotationLicenseResponse(
    uint32_t initial_crypto_period_index,
    uint32_t crypto_period_count) {
  const std::string kTrackTypes[] = {"SD", "HD", "AUDIO"};
  std::string tracks;
  for (uint32_t index = initial_crypto_period_index;
       index < initial_crypto_period_index + crypto_period_count;
       ++index) {
    for (size_t i = 0; i < 3; ++i) {
      if (!tracks.empty())
        tracks += ",";
      tracks += base::StringPrintf(
          kCryptoPeriodTrackFormat,
          kTrackTypes[i].c_str(),
          Base64Encode(GetMockKeyId(kTrackTypes[i])).c_str(),
          Base64Encode(GetMockKey(kTrackTypes[i], index)).c_str(),
          index);
    }
  }
  return base::StringPrintf(kLicenseResponseFormat, "OK", tracks.c_str());
}

}  // namespace

TEST_P(WidevineKeySourceTest, KeyRotationTest) {
  const uint32_t kFirstCryptoPeriodIndex = 8;
  const uint32_t kCryptoPeriodCount = 10;
  // Array of indexes to be checked.
  const uint32_t kCryptoPeriodIndexes[] = {
      kFirstCryptoPeriodIndex, 17, 37, 38, 36, 39};
  // Derived from kCryptoPeriodIndexes: ceiling((39 - 8 ) / 10).
  const uint32_t kCryptoIterations = 4;

  // Generate expectations in sequence.
  InSequence dummy;

  // Expecting a non-key rotation enabled request on FetchKeys().
  EXPECT_CALL(*mock_request_signer_, GenerateSignature(_, _))
      .WillOnce(Return(true));
  std::string mock_response = base::StringPrintf(
      kHttpResponseFormat, Base64Encode(GenerateMockLicenseResponse()).c_str());
  EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));

  for (uint32_t i = 0; i < kCryptoIterations; ++i) {
    uint32_t first_crypto_period_index =
        kFirstCryptoPeriodIndex - 1 + i * kCryptoPeriodCount;
    std::string expected_message =
        base::StringPrintf(kCryptoPeriodRequestMessageFormat,
                           Base64Encode(kContentId).c_str(),
                           kCryptoPeriodCount,
                           first_crypto_period_index,
                           kPolicy);
    EXPECT_CALL(*mock_request_signer_, GenerateSignature(expected_message, _))
        .WillOnce(DoAll(SetArgPointee<1>(kMockSignature), Return(true)));

    std::string mock_response = base::StringPrintf(
        kHttpResponseFormat,
        Base64Encode(GenerateMockKeyRotationLicenseResponse(
                         first_crypto_period_index, kCryptoPeriodCount))
            .c_str());
    EXPECT_CALL(*mock_key_fetcher_, FetchKeys(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(mock_response), Return(Status::OK)));
  }

  CreateWidevineKeySource();
  widevine_key_source_->set_signer(mock_request_signer_.Pass());
  ASSERT_OK(widevine_key_source_->FetchKeys(content_id_, kPolicy));

  EncryptionKey encryption_key;
  for (size_t i = 0; i < arraysize(kCryptoPeriodIndexes); ++i) {
    const std::string kTrackTypes[] = {"SD", "HD", "AUDIO"};
    for (size_t j = 0; j < 3; ++j) {
      ASSERT_OK(widevine_key_source_->GetCryptoPeriodKey(
          kCryptoPeriodIndexes[i],
          KeySource::GetTrackTypeFromString(kTrackTypes[j]),
          &encryption_key));
      EXPECT_EQ(GetMockKey(kTrackTypes[j], kCryptoPeriodIndexes[i]),
                ToString(encryption_key.key));
    }
  }

  // The old crypto period indexes should have been garbage collected.
  Status status = widevine_key_source_->GetCryptoPeriodKey(
      kFirstCryptoPeriodIndex,
      KeySource::TRACK_TYPE_SD,
      &encryption_key);
  EXPECT_EQ(error::INVALID_ARGUMENT, status.error_code());
}

INSTANTIATE_TEST_CASE_P(WidevineKeySourceInstance,
                        WidevineKeySourceTest,
                        ::testing::Bool());

}  // namespace media
}  // namespace shaka
